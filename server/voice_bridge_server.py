"""Core2 LAN bridge: legacy WAV requests and streamed Realtime PCM audio."""
import asyncio
import base64
import hmac
import json
import logging
import os
import io
import math
import struct
import wave
from collections import deque
from datetime import datetime
from typing import Dict, List
from urllib.parse import urlencode

import aiohttp
import requests
from aiohttp import web
from openai import OpenAI

OPENAI_API_KEY = os.getenv("OPENAI_API_KEY", "")
ANTHROPIC_API_KEY = os.getenv("ANTHROPIC_API_KEY", "")
BRIDGE_TOKEN = os.getenv("BRIDGE_TOKEN", "")
OPENAI_TRANSCRIBE_MODEL = os.getenv("OPENAI_TRANSCRIBE_MODEL", "whisper-1")
OPENAI_CHAT_MODEL = os.getenv("OPENAI_CHAT_MODEL", "gpt-4o-mini")
OPENAI_REALTIME_MODEL = os.getenv("OPENAI_REALTIME_MODEL", "gpt-realtime")
OPENAI_REALTIME_VOICE = os.getenv("OPENAI_REALTIME_VOICE", "marin")
OPENAI_TTS_MODEL = os.getenv("OPENAI_TTS_MODEL", "gpt-4o-mini-tts")
OPENAI_TTS_VOICE = os.getenv("OPENAI_TTS_VOICE", "coral")
PCM_VAD_THRESHOLD = max(1, int(os.getenv("PCM_VAD_THRESHOLD", "500")))
ANTHROPIC_MODEL = os.getenv("ANTHROPIC_MODEL", "claude-sonnet-4-6")
ANTHROPIC_MAX_TOKENS = int(os.getenv("ANTHROPIC_MAX_TOKENS", "256"))
SYSTEM_PROMPT = os.getenv("SYSTEM_PROMPT", "You are Stackchan, a friendly desktop companion robot. Keep replies short and spoken Japanese.")
MAX_TURNS = max(1, int(os.getenv("MAX_TURNS", "6")))
SESSION_SECONDS = max(30, int(os.getenv("REALTIME_SESSION_SECONDS", "300")))
VAD_SILENCE_MS = max(200, min(2000, int(os.getenv("VAD_SILENCE_MS", "600"))))
openai_client = OpenAI(api_key=OPENAI_API_KEY, timeout=45, max_retries=1) if OPENAI_API_KEY else None
conversation: Dict[str, Dict[str, List[Dict[str, str]]]] = {"openai": {}, "claude": {}}
log = logging.getLogger("stackchan")
APP_TOKEN = web.AppKey("bridge_token", str)
LEGACY_LOCK = web.AppKey("legacy_lock", asyncio.Lock)
VOICE_STATE = web.AppKey("voice_state", dict)
PCM_FRAME_BYTES = 1920  # 40 ms of mono signed PCM16LE, 24 kHz
AUDIO_WINDOW = 24000   # At most 500 ms awaiting device playback credits

TOOLS = [
    {"type": "function", "name": "start_timer", "description": "Start the robot's single timer. Refuses to replace an active timer.",
     "parameters": {"type": "object", "properties": {"seconds": {"type": "integer", "minimum": 1, "maximum": 86400}, "label": {"type": "string", "maxLength": 40}}, "required": ["seconds"], "additionalProperties": False}},
    *[{"type": "function", "name": name, "description": description,
       "parameters": {"type": "object", "properties": {}, "additionalProperties": False}}
      for name, description in [("timer_status", "Get the robot timer's remaining seconds."), ("cancel_timer", "Cancel the robot timer when requested."), ("get_time", "Get current local time on the bridge PC.")]],
]


def validate_tool(name, arguments):
    if not isinstance(arguments, dict):
        raise ValueError("arguments must be an object")
    if name == "start_timer":
        seconds = arguments.get("seconds")
        label = arguments.get("label", "Timer")
        if type(seconds) is not int or not 1 <= seconds <= 86400:
            raise ValueError("seconds must be an integer from 1 to 86400")
        if not isinstance(label, str) or len(label) > 40 or set(arguments) - {"seconds", "label"}:
            raise ValueError("invalid timer label or fields")
        return {"seconds": seconds, "label": label}
    if name not in {"timer_status", "cancel_timer", "get_time"} or arguments:
        raise ValueError("unsupported tool or fields")
    return {}


def session_update():
    return {"type": "session.update", "session": {
        "type": "realtime", "output_modalities": ["audio"],
        "instructions": SYSTEM_PROMPT + " Use tools for timer and clock requests. Never claim a timer succeeded until the device confirms. You have no access to other devices. Your voice is AI-generated.",
        "audio": {"input": {"format": {"type": "audio/pcm", "rate": 24000},
                            "noise_reduction": {"type": "near_field"},
                            "turn_detection": {"type": "server_vad", "threshold": 0.5, "prefix_padding_ms": 300, "silence_duration_ms": VAD_SILENCE_MS, "create_response": True, "interrupt_response": False}},
                  "output": {"format": {"type": "audio/pcm", "rate": 24000}, "voice": OPENAI_REALTIME_VOICE}},
        "tools": TOOLS, "tool_choice": "auto", "max_output_tokens": 512,
    }}


def _append_history(backend: str, user_id: str, role: str, content: str) -> None:
    history = conversation[backend].setdefault(user_id, [])
    history.append({"role": role, "content": content})
    max_len = MAX_TURNS * 2
    if len(history) > max_len:
        del history[: len(history) - max_len]


def _get_history(backend: str, user_id: str) -> List[Dict[str, str]]:
    return conversation[backend].get(user_id, [])


def transcribe_wav(audio_bytes: bytes) -> str:
    transcription = openai_client.audio.transcriptions.create(
        model=OPENAI_TRANSCRIBE_MODEL,
        file=("recording.wav", audio_bytes, "audio/wav"),
    )
    return transcription.text.strip()


def spoken_reply(backend, user_id, pcm):
    """STT and TTS use OpenAI even when the conversation model is Claude."""
    audio = io.BytesIO()
    with wave.open(audio, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(24000)
        wav.writeframes(pcm)
    transcript = transcribe_wav(audio.getvalue())
    if not transcript:
        return b""
    reply = (chat_claude if backend == "claude" else chat_openai)(user_id, transcript)
    if not reply:
        raise ValueError("empty conversation reply")
    speech = openai_client.audio.speech.create(
        model=OPENAI_TTS_MODEL, voice=OPENAI_TTS_VOICE,
        input=reply, response_format="pcm",
    )
    try:
        pcm = speech.content
    finally:
        speech.close()
    if not pcm or len(pcm) % 2 or len(pcm) > 24000 * 2 * 120:
        raise ValueError("invalid synthesized audio")
    return pcm


def chat_openai(user_id: str, user_text: str) -> str:
    messages = [{"role": "system", "content": SYSTEM_PROMPT}]
    messages.extend(_get_history("openai", user_id))
    messages.append({"role": "user", "content": user_text})

    response = openai_client.chat.completions.create(
        model=OPENAI_CHAT_MODEL,
        messages=messages,
        temperature=0.7,
        max_completion_tokens=512,
    )
    reply = response.choices[0].message.content.strip()

    _append_history("openai", user_id, "user", user_text)
    _append_history("openai", user_id, "assistant", reply)
    return reply


def chat_claude(user_id: str, user_text: str) -> str:
    if not ANTHROPIC_API_KEY:
        raise RuntimeError("ANTHROPIC_API_KEY is required for Claude")

    headers = {
        "x-api-key": ANTHROPIC_API_KEY,
        "anthropic-version": "2023-06-01",
        "content-type": "application/json",
    }
    messages = []
    messages.extend(_get_history("claude", user_id))
    messages.append({"role": "user", "content": user_text})

    payload = {
        "model": ANTHROPIC_MODEL,
        "max_tokens": ANTHROPIC_MAX_TOKENS,
        "system": SYSTEM_PROMPT,
        "messages": messages,
    }

    resp = requests.post("https://api.anthropic.com/v1/messages", headers=headers, json=payload, timeout=60)
    resp.raise_for_status()
    data = resp.json()
    reply = "".join([item.get("text", "") for item in data.get("content", []) if item.get("type") == "text"]).strip()

    _append_history("claude", user_id, "user", user_text)
    _append_history("claude", user_id, "assistant", reply)
    return reply



class RealtimeBridge:
    """One device/session. Credits bound both the device ring and PC audio queue."""
    def __init__(self, device, upstream):
        self.device, self.upstream = device, upstream
        self.credit = AUDIO_WINDOW
        self.credit_changed = asyncio.Event()
        self.audio = asyncio.Queue(maxsize=64)
        self.pending = {}
        self.completed = set()
        self.listening = False
        self.ready = False

    async def send_device(self, kind, **fields):
        await self.device.send_json({"type": kind, **fields})

    async def device_loop(self):
        async for msg in self.device:
            if msg.type == aiohttp.WSMsgType.BINARY:
                if len(msg.data) > PCM_FRAME_BYTES or len(msg.data) % 2:
                    raise ValueError("invalid PCM frame")
                if self.ready and self.listening:
                    await self.upstream.send_json({"type": "input_audio_buffer.append", "audio": base64.b64encode(msg.data).decode("ascii")})
            elif msg.type == aiohttp.WSMsgType.TEXT:
                event = json.loads(msg.data)
                kind = event.get("type")
                if kind == "credit":
                    size = event.get("bytes")
                    if type(size) is not int or size <= 0 or self.credit + size > AUDIO_WINDOW:
                        raise ValueError("invalid audio credit")
                    self.credit += size
                    self.credit_changed.set()
                elif kind == "playback_done" and self.ready and not self.listening:
                    # Drop audio captured during generation/playback; never echo it back.
                    await self.upstream.send_json({"type": "input_audio_buffer.clear"})
                    self.listening = True
                    await self.send_device("listening")
                elif kind == "tool_result":
                    future = self.pending.get(event.get("call_id"))
                    if future and not future.done() and isinstance(event.get("result"), dict):
                        future.set_result(event["result"])
                elif kind == "stop":
                    return
            elif msg.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.ERROR):
                return

    async def audio_loop(self):
        while True:
            chunk = await self.audio.get()
            if chunk is None:
                await self.send_device("audio_end")
                continue
            while self.credit < len(chunk):
                self.credit_changed.clear()
                await asyncio.wait_for(self.credit_changed.wait(), timeout=15)
            self.credit -= len(chunk)
            await self.device.send_bytes(chunk)

    async def call_tool(self, call):
        call_id = call["call_id"]
        if call_id in self.completed:
            return
        self.completed.add(call_id)
        try:
            name = call["name"]
            arguments = validate_tool(name, json.loads(call["arguments"]))
            if name == "get_time":
                result = {"time": datetime.now().astimezone().isoformat(timespec="seconds")}
            else:
                future = asyncio.get_running_loop().create_future()
                self.pending[call_id] = future
                await self.send_device("tool_call", call_id=call_id, name=name, arguments=arguments)
                result = await asyncio.wait_for(future, timeout=8)
        except (ValueError, KeyError, TypeError):
            result = {"error": "invalid tool arguments"}
        except asyncio.TimeoutError:
            result = {"error": "device did not confirm; timer state is unknown"}
        finally:
            self.pending.pop(call_id, None)
        await self.upstream.send_json({"type": "conversation.item.create", "item": {
            "type": "function_call_output", "call_id": call_id, "output": json.dumps(result, ensure_ascii=False)}})

    async def upstream_loop(self):
        await self.upstream.send_json(session_update())
        async for msg in self.upstream:
            if msg.type == aiohttp.WSMsgType.ERROR:
                raise ConnectionError("upstream closed")
            if msg.type != aiohttp.WSMsgType.TEXT:
                continue
            event = json.loads(msg.data)
            kind = event.get("type")
            if kind == "session.updated" and not self.ready:
                self.ready = self.listening = True
                await self.send_device("ready", sample_rate=24000, window=AUDIO_WINDOW)
            elif kind in {"input_audio_buffer.speech_stopped", "response.created"}:
                self.listening = False
                await self.send_device("thinking")
            elif kind == "input_audio_buffer.speech_started":
                await self.send_device("speech_started")
            elif kind == "response.output_audio.delta":
                data = base64.b64decode(event["delta"], validate=True)
                if len(data) % 2:
                    raise ValueError("invalid upstream PCM")
                for pos in range(0, len(data), PCM_FRAME_BYTES):
                    await self.audio.put(data[pos:pos + PCM_FRAME_BYTES])
            elif kind == "response.done":
                response = event.get("response", {})
                if response.get("status") in {"failed", "cancelled", "incomplete"}:
                    raise RuntimeError("upstream response did not complete")
                calls = [item for item in response.get("output", []) if item.get("type") == "function_call"]
                if calls:
                    for call in calls:
                        await self.call_tool(call)
                    await self.upstream.send_json({"type": "response.create"})
                else:
                    await self.audio.put(None)
            elif kind == "error":
                # Do not send provider payloads, which may contain input, to logs/UI.
                log.warning("Realtime provider rejected an event")
                raise RuntimeError("Realtime provider rejected the session")

    async def run(self):
        tasks = [asyncio.create_task(method()) for method in (self.device_loop, self.audio_loop, self.upstream_loop)]
        try:
            done, _ = await asyncio.wait(tasks, timeout=SESSION_SECONDS, return_when=asyncio.FIRST_COMPLETED)
            if not done:
                await self.send_device("error", message="Session time limit. Press A to reconnect.")
            for task in done:
                task.result()
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)


class ChatBridge(RealtimeBridge):
    """Turn-based STT/LLM/TTS using the same device PCM and credit protocol."""
    def __init__(self, device, backend, user_id):
        super().__init__(device, None)
        self.backend, self.user_id = backend, user_id
        self.prefix = deque(maxlen=8)
        self.recording = bytearray()
        self.quiet_samples = 0
        self.response_task = None

    async def upstream_loop(self):
        # The parent run method also supervises the audio and device loops.
        await asyncio.Future()

    async def respond(self, pcm):
        try:
            result = await asyncio.to_thread(spoken_reply, self.backend, self.user_id, pcm)
            for pos in range(0, len(result), PCM_FRAME_BYTES):
                await self.audio.put(result[pos:pos + PCM_FRAME_BYTES])
            await self.audio.put(None)
        except Exception:
            log.warning("Conversation provider request failed")
            await self.send_device("error", message="Conversation failed. Check API keys/model and press A to retry.")
            await self.device.close()

    async def capture(self, pcm):
        samples = struct.unpack("<%dh" % (len(pcm) // 2), pcm)
        loud = math.sqrt(sum(s * s for s in samples) / len(samples)) >= PCM_VAD_THRESHOLD
        if not self.recording:
            self.prefix.append(pcm)
            if not loud:
                return
            self.recording.extend(b"".join(self.prefix))
            self.prefix.clear()
            await self.send_device("speech_started")
        else:
            self.recording.extend(pcm)
        self.quiet_samples = 0 if loud else self.quiet_samples + len(samples)
        if self.quiet_samples >= 24 * VAD_SILENCE_MS or len(self.recording) >= 24000 * 2 * 15:
            self.listening = False
            await self.send_device("thinking")
            pcm = bytes(self.recording)
            self.recording.clear()
            self.quiet_samples = 0
            self.response_task = asyncio.create_task(self.respond(pcm))

    async def device_loop(self):
        self.ready = self.listening = True
        await self.send_device("ready", sample_rate=24000, window=AUDIO_WINDOW)
        async for msg in self.device:
            if msg.type == aiohttp.WSMsgType.BINARY:
                if not msg.data or len(msg.data) > PCM_FRAME_BYTES or len(msg.data) % 2:
                    raise ValueError("invalid PCM frame")
                if self.listening:
                    await self.capture(msg.data)
            elif msg.type == aiohttp.WSMsgType.TEXT:
                event = json.loads(msg.data)
                if event.get("type") == "credit":
                    size = event.get("bytes")
                    if type(size) is not int or size <= 0 or self.credit + size > AUDIO_WINDOW:
                        raise ValueError("invalid audio credit")
                    self.credit += size
                    self.credit_changed.set()
                elif event.get("type") == "playback_done" and not self.listening:
                    # Only resume once generation and all queued audio have completed.
                    if self.response_task and not self.response_task.done():
                        continue
                    self.listening = True
                    await self.send_device("listening")
                elif event.get("type") == "stop":
                    return
            elif msg.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.ERROR):
                return

    async def run(self):
        try:
            await super().run()
        finally:
            if self.response_task:
                self.response_task.cancel()
                await asyncio.gather(self.response_task, return_exceptions=True)


@web.middleware
async def authentication(request, handler):
    if request.path == "/health":
        return await handler(request)
    expected = request.app[APP_TOKEN]
    if not hmac.compare_digest(request.headers.get("Authorization", ""), "Bearer " + expected):
        return web.json_response({"error": "unauthorized"}, status=401)
    return await handler(request)


async def chat_endpoint(request):
    backend = request.headers.get("X-Backend", "openai").lower()
    if backend not in conversation:
        return web.json_response({"error": "unsupported backend"}, status=400)
    if openai_client is None:
        return web.json_response({"error": "OPENAI_API_KEY is required"}, status=503)
    audio = await request.read()
    if len(audio) < 44 or audio[:4] != b"RIFF" or audio[8:12] != b"WAVE":
        return web.json_response({"error": "expected WAV audio"}, status=400)
    user_id = request.headers.get("X-User-Id", "default")[:128]
    # Serialize legacy history updates and bound API concurrency.
    async with request.app[LEGACY_LOCK]:
        try:
            transcript = await asyncio.to_thread(transcribe_wav, audio)
            if not transcript:
                return web.json_response({"error": "no speech"}, status=422)
            reply = await asyncio.to_thread(chat_claude if backend == "claude" else chat_openai, user_id, transcript)
            return web.json_response({"backend": backend, "transcript": transcript, "text": reply})
        except Exception:
            log.warning("Legacy provider request failed")
            return web.json_response({"error": "provider request failed"}, status=502)


async def realtime_endpoint(request):
    if not OPENAI_API_KEY:
        return web.json_response({"error": "OPENAI_API_KEY is required"}, status=503)
    # One physical robot per bridge, avoiding duplicate sessions/cost on reconnect.
    if request.app[VOICE_STATE]["busy"]:
        return web.json_response({"error": "robot already connected"}, status=409)
    request.app[VOICE_STATE]["busy"] = True
    device = web.WebSocketResponse(heartbeat=20, max_msg_size=4096)
    try:
        await device.prepare(request)
        timeout = aiohttp.ClientTimeout(total=None, connect=15, sock_connect=15)
        async with aiohttp.ClientSession(timeout=timeout) as http:
            url = "wss://api.openai.com/v1/realtime?" + urlencode({"model": OPENAI_REALTIME_MODEL})
            async with http.ws_connect(url, headers={"Authorization": "Bearer " + OPENAI_API_KEY}, heartbeat=20, max_msg_size=2**20) as upstream:
                await RealtimeBridge(device, upstream).run()
    except Exception:
        log.warning("Realtime connection ended with an error")
        if device.prepared and not device.closed:
            await device.send_json({"type": "error", "message": "Voice connection failed. Check bridge configuration and retry A."})
    finally:
        request.app[VOICE_STATE]["busy"] = False
        if device.prepared:
            await device.close()
    return device


async def voice_endpoint(request):
    backend = request.query.get("backend", "openai")
    if backend not in conversation:
        return web.json_response({"error": "unsupported backend"}, status=400)
    if not OPENAI_API_KEY or (backend == "claude" and not ANTHROPIC_API_KEY):
        return web.json_response({"error": "required API key is missing"}, status=503)
    if request.app[VOICE_STATE]["busy"]:
        return web.json_response({"error": "robot already connected"}, status=409)
    request.app[VOICE_STATE]["busy"] = True
    device = web.WebSocketResponse(heartbeat=20, max_msg_size=4096)
    try:
        await device.prepare(request)
        user_id = request.headers.get("X-User-Id", "core2")[:128]
        await ChatBridge(device, backend, user_id).run()
    except Exception:
        log.warning("Voice connection ended with an error")
    finally:
        request.app[VOICE_STATE]["busy"] = False
        if device.prepared:
            await device.close()
    return device


async def health(request):
    return web.json_response({"status": "ok", "realtime_configured": bool(OPENAI_API_KEY)})


def create_app(token=None):
    token = BRIDGE_TOKEN if token is None else token
    if len(token) < 16:
        raise ValueError("Set BRIDGE_TOKEN to at least 16 characters and enter the same token in the robot Web settings")
    app = web.Application(middlewares=[authentication], client_max_size=1024 * 1024)
    app[APP_TOKEN] = token
    app[LEGACY_LOCK] = asyncio.Lock()
    app[VOICE_STATE] = {"busy": False}
    app.add_routes([web.get("/health", health), web.post("/v1/chat", chat_endpoint), web.get("/v1/realtime", realtime_endpoint), web.get("/v1/voice", voice_endpoint)])
    return app


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    web.run_app(create_app(), host=os.getenv("HOST", "0.0.0.0"), port=int(os.getenv("PORT", "8000")), access_log=None)
