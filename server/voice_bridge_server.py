import os
import tempfile
from typing import Dict, List

import requests
from flask import Flask, jsonify, request
from openai import OpenAI

app = Flask(__name__)

OPENAI_API_KEY = os.getenv("OPENAI_API_KEY", "")
ANTHROPIC_API_KEY = os.getenv("ANTHROPIC_API_KEY", "")

OPENAI_TRANSCRIBE_MODEL = os.getenv("OPENAI_TRANSCRIBE_MODEL", "whisper-1")
OPENAI_CHAT_MODEL = os.getenv("OPENAI_CHAT_MODEL", "gpt-4o-mini")
ANTHROPIC_MODEL = os.getenv("ANTHROPIC_MODEL", "claude-sonnet-4-20250514")
ANTHROPIC_MAX_TOKENS = int(os.getenv("ANTHROPIC_MAX_TOKENS", "256"))
SYSTEM_PROMPT = os.getenv("SYSTEM_PROMPT", "You are Stackchan, a friendly desktop companion robot. Keep replies short and spoken Japanese.")
MAX_TURNS = int(os.getenv("MAX_TURNS", "6"))

if not OPENAI_API_KEY:
    raise RuntimeError("OPENAI_API_KEY is required")

openai_client = OpenAI(api_key=OPENAI_API_KEY)

conversation: Dict[str, Dict[str, List[Dict[str, str]]]] = {
    "openai": {},
    "claude": {},
}


def _append_history(backend: str, user_id: str, role: str, content: str) -> None:
    history = conversation[backend].setdefault(user_id, [])
    history.append({"role": role, "content": content})
    max_len = MAX_TURNS * 2
    if len(history) > max_len:
        del history[: len(history) - max_len]


def _get_history(backend: str, user_id: str) -> List[Dict[str, str]]:
    return conversation[backend].get(user_id, [])


def transcribe_wav(audio_bytes: bytes) -> str:
    with tempfile.NamedTemporaryFile(suffix=".wav") as tmp:
        tmp.write(audio_bytes)
        tmp.flush()
        transcription = openai_client.audio.transcriptions.create(
            model=OPENAI_TRANSCRIBE_MODEL,
            file=tmp,
        )
    return transcription.text.strip()


def chat_openai(user_id: str, user_text: str) -> str:
    messages = [{"role": "system", "content": SYSTEM_PROMPT}]
    messages.extend(_get_history("openai", user_id))
    messages.append({"role": "user", "content": user_text})

    response = openai_client.chat.completions.create(
        model=OPENAI_CHAT_MODEL,
        messages=messages,
        temperature=0.7,
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


@app.post("/v1/chat")
def chat_endpoint():
    backend = request.headers.get("X-Backend", "openai").lower()
    user_id = request.headers.get("X-User-Id", "default")
    audio_bytes = request.get_data() or b""

    if not audio_bytes:
        return jsonify({"error": "empty audio"}), 400

    transcript = transcribe_wav(audio_bytes)
    if not transcript:
        return jsonify({"error": "transcription failed"}), 500

    if backend == "claude":
        reply = chat_claude(user_id, transcript)
    else:
        reply = chat_openai(user_id, transcript)

    return jsonify({
        "backend": backend,
        "transcript": transcript,
        "text": reply,
    })


if __name__ == "__main__":
    port = int(os.getenv("PORT", "8000"))
    app.run(host="0.0.0.0", port=port)
