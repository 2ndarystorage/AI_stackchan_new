"""Offline protocol tests; no API key, paid requests, or robot required."""
import asyncio
import base64
import json
import struct
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import aiohttp
from aiohttp.test_utils import AioHTTPTestCase
import voice_bridge_server as bridge


class FakeSocket:
    def __init__(self):
        self.input = asyncio.Queue()
        self.sent = []

    def __aiter__(self):
        return self

    async def __anext__(self):
        item = await self.input.get()
        if item is None:
            raise StopAsyncIteration
        return item

    async def send_json(self, data):
        self.sent.append(data)

    async def send_bytes(self, data):
        self.sent.append(data)

    def event(self, event):
        self.input.put_nowait(SimpleNamespace(type=aiohttp.WSMsgType.TEXT, data=json.dumps(event)))


class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def test_audio_credit_blocks_without_dropping_pcm(self):
        device, upstream = FakeSocket(), FakeSocket()
        relay = bridge.RealtimeBridge(device, upstream)
        relay.credit = 0
        task = asyncio.create_task(relay.audio_loop())
        try:
            await relay.audio.put(b"\x00\x01" * 960)
            await asyncio.sleep(0.01)
            self.assertEqual(device.sent, [])
            relay.credit = 1920
            relay.credit_changed.set()
            await asyncio.sleep(0.01)
            self.assertEqual(device.sent, [b"\x00\x01" * 960])
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    async def test_tool_requires_device_confirmation(self):
        device, upstream = FakeSocket(), FakeSocket()
        relay = bridge.RealtimeBridge(device, upstream)
        call = {"call_id": "one", "name": "start_timer", "arguments": '{"seconds":180}'}
        task = asyncio.create_task(relay.call_tool(call))
        await asyncio.sleep(0)
        self.assertEqual(device.sent[0]["arguments"]["seconds"], 180)
        self.assertEqual(upstream.sent, [])
        relay.pending["one"].set_result({"ok": True, "remaining_seconds": 180})
        await task
        result = json.loads(upstream.sent[0]["item"]["output"])
        self.assertTrue(result["ok"])
        await relay.call_tool(call)
        self.assertEqual(len(device.sent), 1)

    async def test_unsupported_or_malformed_tools_do_not_reach_device(self):
        device, upstream = FakeSocket(), FakeSocket()
        relay = bridge.RealtimeBridge(device, upstream)
        for i, (name, args) in enumerate([("shell", {}), ("start_timer", {"seconds": True}), ("start_timer", {"seconds": 86401}), ("cancel_timer", {"extra": 1})]):
            await relay.call_tool({"call_id": str(i), "name": name, "arguments": json.dumps(args)})
        self.assertEqual(device.sent, [])
        self.assertEqual(len(upstream.sent), 4)
        self.assertTrue(all("error" in json.loads(x["item"]["output"]) for x in upstream.sent))

    async def test_ready_audio_and_end_order(self):
        device, upstream = FakeSocket(), FakeSocket()
        relay = bridge.RealtimeBridge(device, upstream)
        upstream.event({"type": "session.updated"})
        upstream.event({"type": "response.created"})
        upstream.event({"type": "response.output_audio.delta", "delta": base64.b64encode(b"\x01\x02" * 1200).decode()})
        upstream.event({"type": "response.done", "response": {"status": "completed", "output": []}})
        upstream.input.put_nowait(None)
        await relay.upstream_loop()
        self.assertEqual(device.sent[0]["type"], "ready")
        self.assertFalse(relay.listening)
        self.assertEqual(len(await relay.audio.get()), 1920)
        self.assertEqual(len(await relay.audio.get()), 480)
        self.assertIsNone(await relay.audio.get())

    async def test_microphone_is_ignored_until_session_ready_and_while_speaking(self):
        device, upstream = FakeSocket(), FakeSocket()
        relay = bridge.RealtimeBridge(device, upstream)
        pcm = SimpleNamespace(type=aiohttp.WSMsgType.BINARY, data=b"\x00\x00" * 960)
        device.input.put_nowait(pcm)
        device.input.put_nowait(None)
        await relay.device_loop()
        self.assertEqual(upstream.sent, [])
        relay.ready = relay.listening = True
        device.input.put_nowait(pcm)
        device.input.put_nowait(None)
        await relay.device_loop()
        self.assertEqual(upstream.sent[0]["type"], "input_audio_buffer.append")


class HTTPTests(AioHTTPTestCase):
    async def get_application(self):
        return bridge.create_app("test-secret-123456789")

    async def test_health_and_authentication(self):
        response = await self.client.get("/health")
        self.assertEqual(response.status, 200)
        for path in ("/v1/chat", "/v1/realtime", "/v1/voice"):
            response = await self.client.request("POST" if path.endswith("chat") else "GET", path)
            self.assertEqual(response.status, 401)

    async def test_rejects_invalid_wav_and_unknown_backend(self):
        headers = {"Authorization": "Bearer test-secret-123456789"}
        with patch.object(bridge, "openai_client", object()):
            response = await self.client.post("/v1/chat", headers=headers, data=b"bad")
            self.assertEqual(response.status, 400)
            response = await self.client.post("/v1/chat", headers={**headers, "X-Backend": "unknown"}, data=b"bad")
            self.assertEqual(response.status, 400)

    async def test_voice_websocket_round_trip_and_resume(self):
        headers = {"Authorization": "Bearer test-secret-123456789"}
        pcm = struct.pack("<960h", *([1200] * 960))
        silence = b"\x00\x00" * 960
        with patch.object(bridge, "OPENAI_API_KEY", "test"), \
             patch.object(bridge, "ANTHROPIC_API_KEY", "test"), \
             patch.object(bridge, "spoken_reply", return_value=pcm) as provider:
            ws = await self.client.ws_connect("/v1/voice?backend=claude", headers=headers)
            try:
                self.assertEqual((await ws.receive_json())["type"], "ready")
                await ws.send_bytes(pcm)
                self.assertEqual((await ws.receive_json())["type"], "speech_started")
                for _ in range((bridge.VAD_SILENCE_MS + 39) // 40):
                    await ws.send_bytes(silence)
                self.assertEqual((await ws.receive_json())["type"], "thinking")
                self.assertEqual((await ws.receive()).data, pcm)
                self.assertEqual((await ws.receive_json())["type"], "audio_end")
                await ws.send_json({"type": "credit", "bytes": len(pcm)})
                await ws.send_json({"type": "playback_done"})
                self.assertEqual((await ws.receive_json())["type"], "listening")
                self.assertEqual(provider.call_args.args[0], "claude")
                await ws.send_json({"type": "stop"})
                self.assertEqual((await ws.receive()).type, aiohttp.WSMsgType.CLOSE)
            finally:
                await ws.close()
            self.assertFalse(self.app[bridge.VOICE_STATE]["busy"])

    async def test_voice_rejects_unknown_backend_and_missing_keys(self):
        headers = {"Authorization": "Bearer test-secret-123456789"}
        response = await self.client.get("/v1/voice?backend=unknown", headers=headers)
        self.assertEqual(response.status, 400)
        with patch.object(bridge, "OPENAI_API_KEY", ""):
            response = await self.client.get("/v1/voice?backend=claude", headers=headers)
            self.assertEqual(response.status, 503)


class ChatVoiceTests(unittest.IsolatedAsyncioTestCase):
    async def test_silence_does_not_call_provider_and_speech_waits_for_playback(self):
        device = FakeSocket()
        relay = bridge.ChatBridge(device, "claude", "test")
        relay.ready = relay.listening = True
        silence = b"\x00\x00" * 960
        speech = struct.pack("<960h", *([1200] * 960))
        with patch.object(bridge, "spoken_reply", return_value=b"\x01\x00" * 1200) as provider:
            for _ in range(20):
                await relay.capture(silence)
            provider.assert_not_called()
            await relay.capture(speech)
            for _ in range((bridge.VAD_SILENCE_MS + 39) // 40):
                await relay.capture(silence)
            await relay.response_task
            self.assertFalse(relay.listening)
            self.assertEqual(provider.call_args.args[:2], ("claude", "test"))
            self.assertLessEqual(len(provider.call_args.args[2]), 24000 * 2 * 16)
            self.assertEqual(len(await relay.audio.get()), 1920)
            self.assertEqual(len(await relay.audio.get()), 480)
            self.assertIsNone(await relay.audio.get())

    async def test_playback_done_resumes_listening(self):
        device = FakeSocket()
        relay = bridge.ChatBridge(device, "openai", "test")
        device.event({"type": "playback_done"})
        device.input.put_nowait(None)
        # device_loop starts listening; simulate the completed turn after ready.
        original = relay.send_device
        async def send(kind, **fields):
            await original(kind, **fields)
            if kind == "ready":
                relay.listening = False
        relay.send_device = send
        await relay.device_loop()
        self.assertTrue(relay.listening)
        self.assertEqual([x["type"] for x in device.sent], ["ready", "listening"])

    async def test_speech_pipeline_selects_backend_and_builds_wav(self):
        from types import SimpleNamespace
        pcm = b"\x00\x00" * 960
        speech = SimpleNamespace(content=pcm, close=lambda: None)
        client = SimpleNamespace(audio=SimpleNamespace(speech=SimpleNamespace(create=lambda **kw: speech)))
        with patch.object(bridge, "openai_client", client), \
             patch.object(bridge, "transcribe_wav", return_value="こんにちは") as stt, \
             patch.object(bridge, "chat_claude", return_value="こんにちは！") as claude, \
             patch.object(bridge, "chat_openai") as openai:
            self.assertEqual(bridge.spoken_reply("claude", "test", pcm), pcm)
            self.assertEqual(stt.call_args.args[0][:4], b"RIFF")
            claude.assert_called_once_with("test", "こんにちは")
            openai.assert_not_called()


if __name__ == "__main__":
    unittest.main()
