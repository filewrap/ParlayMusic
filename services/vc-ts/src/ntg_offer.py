"""ntgcalls local-offer probe for vc-ts / vc-rust.

Runs ntgcalls create_call(chat_id) and prints one JSON object:
  {"ok": true, "offer_len": N, "offer": "<local SDP params>"}
or {"ok": false, "error": "<ExceptionType: message>"}.

This is the TinaMusic Play() step 1 (voice_binding_.CreateCall): it is
purely local WebRTC — no MTProto, no network, no credentials. It proves
the ntgcalls engine can build a call and produce the offer that
phone.joinGroupCall would carry. The join itself needs the assistant
MTProto session (core-cpp TdLibVoiceCall, in-process) and is NOT done
here. This probe is the fallback for the in-process N-API addon
(services/vc-ts/src/ntg_addon.cc) when libntgcalls.so (or the addon)
is absent; the .so is built now (phase5/ntg-build.log), so the addon
is the live path and this probe is only the fallback.
"""
import asyncio
import json
import sys

from ntgcalls import NTgCalls


async def main(chat_id):
    ntg = NTgCalls()
    try:
        offer = await ntg.create_call(chat_id)
        print(json.dumps({"ok": True, "offer_len": len(offer), "offer": offer}))
        return 0
    except Exception as e:  # noqa: BLE001 - report the exact failure
        print(json.dumps({"ok": False, "error": f"{type(e).__name__}: {e}"}))
        return 1
    finally:
        try:
            await ntg.stop(chat_id)
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(asyncio.run(main(int(sys.argv[1]) if len(sys.argv) > 1 else -1004242)))
