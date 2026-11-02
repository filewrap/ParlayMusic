#!/usr/bin/env python3
"""Parlay voice bridge — Telethon (MTProto, assistant user account) +
ntgcalls (WebRTC). This is the TinaMusic voice path, in Python, because
that is where working ntgcalls + Telethon bindings exist in this sandbox:

    TinaProtoClient::Play(chat_id, stream_url):
      1. BuildFfmpegCommand(stream_url)            -> --ffmpeg-cmd
      2. voice_binding_.CreateCall(chat_id)        -> ntgcalls.create_call
      3. SetStreamSources(chat_id, cmd)            -> ntgcalls.set_stream_sources
      4. mtproto_.JoinGroupCall(chat_id, local)    -> phone.joinGroupCall
      5. voice_binding_.Connect(chat_id, remote)   -> ntgcalls.connect

Modes:
  join-call --chat-id N --session FILE --api-id N --api-hash X
            --ffmpeg-cmd CMD [--video-cmd CMD]
      Full join + stream until the stream ends or SIGTERM. Prints one JSON
      object per line on stdout: {"event": "started"|"stream_end"|"error",
      ...}. Exit 0 on a clean end/stop, 1 on failure.
  join-chat --chat-id N --session FILE --api-id N --api-hash X
            --invite LINK
      Assistant joins the chat via the invite link (NoxBeats
      ensure_assistant: invite -> assistant_join). Prints one JSON object
      {"ok": true|false, ...} and exits 0/1.

The session file holds a Telethon StringSession string (0600, never
logged). No token/secret is printed: errors are sanitised to their type.

NOTE: the sandbox has no direct TCP route to Telegram's DCs (egress is a
proxy; MTProto is raw TCP), so Telethon connect fails here with a
connection error. That is an environment limit, not a code gap: every step
up to the network call is exercised, and ntgcalls create_call /
set_stream_sources are proven offline (see phase4/worker-a.md).
"""
import argparse
import asyncio
import json
import signal
import sys

from ntgcalls import (
    AudioDescription,
    MediaDescription,
    MediaSource,
    NTgCalls,
    StreamMode,
    VideoDescription,
)


def log(obj):
    print(json.dumps(obj), flush=True)


def sanitise(exc):
    # Never leak session material or full tracebacks into logs.
    return f"{type(exc).__name__}"


def read_session(path):
    with open(path, "r", encoding="utf-8") as f:
        s = f.read().strip()
    if not s:
        raise ValueError("empty session file")
    return s


def invite_hash(link):
    s = link.strip()
    for p in ("tg://join?invite=", "https://t.me/+", "http://t.me/+",
              "https://t.me/joinchat/", "http://t.me/joinchat/"):
        if s.startswith(p):
            return s[len(p):]
    if s.startswith("+"):
        return s[1:]
    return ""


def find_connection(updates):
    """NoxBeats find_connection(): updateGroupCallConnection (not the
    presentation one) -> params.data."""
    for u in getattr(updates, "updates", []) or []:
        if type(u).__name__ == "UpdateGroupCallConnection":
            if getattr(u, "presentation", False):
                continue
            params = getattr(u, "params", None)
            data = getattr(params, "data", "") if params else ""
            if data:
                return data
    return ""


async def get_call(client, chat_id):
    """Return (input_group_call, peer) for the chat's active group call,
    creating one when there is none. Mirrors the assistant's
    joinGroupCall precondition in NoxBeats/TinaMusic."""
    from telethon.tl.functions.channels import GetFullChannelRequest
    from telethon.tl.functions.messages import GetFullChatRequest
    from telethon.tl.functions.phone import CreateGroupCallRequest
    from telethon.tl.types import InputGroupCall

    entity = await client.get_entity(chat_id)
    full = None
    try:
        full = await client(GetFullChannelRequest(entity))
        chat_full = full.full_chat
    except Exception:
        full = await client(GetFullChatRequest(chat_id))
        chat_full = full.full_chat
    call = getattr(chat_full, "call", None)
    if call is not None:
        return call, entity
    # No active call: the assistant creates one (needs to be able to
    # manage the voice chat; otherwise Telegram returns an error here).
    import random
    res = await client(CreateGroupCallRequest(
        peer=entity, random_id=random.randint(1, 2 ** 31 - 1)))
    for u in getattr(res, "updates", []) or []:
        if type(u).__name__ == "UpdateGroupCall":
            c = getattr(u, "call", None)
            if c is not None:
                return InputGroupCall(id=c.id, access_hash=c.access_hash), entity
    raise RuntimeError("could not create a group call")


async def cmd_join_call(args):
    from telethon import TelegramClient
    from telethon.sessions import StringSession
    from telethon.tl.functions.phone import JoinGroupCallRequest, LeaveGroupCallRequest
    from telethon.tl.types import DataJSON

    session = read_session(args.session)
    client = TelegramClient(StringSession(session), args.api_id, args.api_hash)
    ntg = NTgCalls()
    stop = asyncio.Event()

    def on_signal(*_a):
        stop.set()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            loop.add_signal_handler(sig, on_signal)
        except NotImplementedError:
            pass

    call = None
    joined = False
    try:
        await client.connect()
        if not await client.is_user_authorized():
            raise RuntimeError("assistant session is not authorized")
        me = await client.get_me()
        log({"event": "authorized", "user_id": me.id})

        offer = await ntg.create_call(args.chat_id)
        log({"event": "offer_created", "len": len(offer)})

        audio = AudioDescription(MediaSource.SHELL, 48000, 2,
                                 args.ffmpeg_cmd, False)
        video = None
        if args.video_cmd:
            video = VideoDescription(
                MediaSource.SHELL, 1280, 720, 30, args.video_cmd, False)
        desc = MediaDescription(audio, None, video, None)
        await ntg.set_stream_sources(args.chat_id, StreamMode.CAPTURE, desc)
        log({"event": "sources_set"})

        call, _peer = await get_call(client, args.chat_id)
        join_as = await client.get_input_entity("me")
        res = await client(JoinGroupCallRequest(
            call=call, join_as=join_as,
            params=DataJSON(data=offer), muted=False, video_stopped=True))
        remote = find_connection(res)
        if not remote:
            raise RuntimeError("joinGroupCall returned no transport")
        log({"event": "joined", "transport_len": len(remote)})

        await ntg.connect(args.chat_id, remote)
        joined = True
        log({"event": "connected"})

        stream_ended = asyncio.Event()

        def _on_end(chat_id, _stream_type, _device):
            if chat_id == args.chat_id:
                loop.call_soon_threadsafe(stream_ended.set)

        ntg.on_stream_end(_on_end)

        done, _pending = await asyncio.wait(
            [asyncio.create_task(stop.wait()),
             asyncio.create_task(stream_ended.wait())],
            return_when=asyncio.FIRST_COMPLETED)
        if stream_ended.is_set():
            log({"event": "stream_end"})
        else:
            log({"event": "stopped"})
        return 0
    except Exception as e:  # noqa: BLE001 - report, don't crash the core
        log({"event": "error", "error": sanitise(e)})
        return 1
    finally:
        try:
            if joined and call is not None:
                import random
                try:
                    await client(LeaveGroupCallRequest(call=call, source=random.randint(1, 2 ** 31 - 1)))
                except Exception:
                    pass
            await ntg.stop(args.chat_id)
        except Exception:
            pass
        await client.disconnect()


async def cmd_join_chat(args):
    from telethon import TelegramClient
    from telethon.sessions import StringSession
    from telethon.tl.functions.channels import JoinChannelRequest
    from telethon.tl.functions.messages import ImportChatInviteRequest

    session = read_session(args.session)
    client = TelegramClient(StringSession(session), args.api_id, args.api_hash)
    try:
        await client.connect()
        if not await client.is_user_authorized():
            print(json.dumps({"ok": False, "error": "session not authorized"}))
            return 1
        link = args.invite or ""
        if link.startswith("@"):
            await client(JoinChannelRequest(link))
        else:
            h = invite_hash(link)
            if not h:
                print(json.dumps({"ok": False, "error": "bad invite link"}))
                return 1
            await client(ImportChatInviteRequest(h))
        print(json.dumps({"ok": True, "chat_id": args.chat_id}))
        return 0
    except Exception as e:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": sanitise(e)}))
        return 1
    finally:
        await client.disconnect()


def main():
    p = argparse.ArgumentParser(description="Parlay voice bridge")
    sub = p.add_subparsers(dest="cmd", required=True)
    jc = sub.add_parser("join-call")
    jc.add_argument("--chat-id", type=int, required=True)
    jc.add_argument("--session", required=True)
    jc.add_argument("--api-id", type=int, required=True)
    jc.add_argument("--api-hash", required=True)
    jc.add_argument("--ffmpeg-cmd", required=True)
    jc.add_argument("--video-cmd", default="")
    jch = sub.add_parser("join-chat")
    jch.add_argument("--chat-id", type=int, required=True)
    jch.add_argument("--session", required=True)
    jch.add_argument("--api-id", type=int, required=True)
    jch.add_argument("--api-hash", required=True)
    jch.add_argument("--invite", default="")
    args = p.parse_args()
    try:
        if args.cmd == "join-call":
            return asyncio.run(cmd_join_call(args))
        return asyncio.run(cmd_join_chat(args))
    except Exception as e:  # noqa: BLE001 - never leak a traceback to the core
        print(json.dumps({"ok": False, "error": sanitise(e)}))
        return 1


if __name__ == "__main__":
    sys.exit(main())
