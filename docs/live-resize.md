# Live resize

Eddyshine accepts a mid-session size change from the client and resizes the captured game window. The stream is not restarted. The display mode is not changed.

Stock Moonlight does not send `0x5506` and is not sent `0x5507`. A client that ignores `0x5507` can freeze, so the host stays quiet until the client opts in.

## Capability

`/serverinfo` on HTTP and HTTPS includes:

```xml
<ClientResolutionChange>1</ClientResolutionChange>
```

Send `0x5506` only when that field is `1`.

## Client request (`0x5506`)

Little-endian plaintext, before encryption. 16 bytes:

| Offset | Type | Value |
| --- | --- | --- |
| 0 | uint16 | `0x5506` |
| 2 | uint16 | `12` (payload length) |
| 4 | int32 | `0` (`SS_DYNAMIC_PARAM_TYPE_RESOLUTION`) |
| 8 | int32 | width |
| 12 | int32 | height |

Example, 1280×720: `06 55 0c 00 00 00 00 00 00 05 00 00 d0 02 00 00`

On an encrypted control stream (protocol 13, what current Moonlight uses) the host drops a bare `0x5506`. Encrypt this plaintext with the session key the same way as every other control message, inside the `0x0001` wrapper. After decryption the host reads the uint16 type, strips the 4-byte header, and the handler sees the 12-byte body (`param_type`, width, height). Extra body bytes are ignored.

Other dynamic-parameter types (FPS, bitrate, QP, SDR white level) are ignored.

The first accepted `0x5506` opts that session into `0x5507`.

## Host notification (`0x5507`)

Little-endian plaintext, before encryption. 12 bytes. Sent only after the session has opted in, and only when the encode size actually changes.

| Offset | Type | Value |
| --- | --- | --- |
| 0 | uint16 | `0x5507` |
| 2 | uint16 | `8` |
| 4 | uint32 | width |
| 8 | uint32 | height |

Example, 1280×720: `07 55 08 00 00 05 00 00 d0 02 00 00`

The host encrypts it the same way it sends HDR mode (`0x010e`). The client should switch its decoder to this size, then expect an IDR. The host waits about 200 ms after queueing `0x5507` before requesting that IDR, and encoder setup adds more delay, so the notification leaves before the new keyframe.

The numbers are the encode size, which is the clamped request when the window followed. They are not a display mode.

## What the host does

1. Debounce. The first size is applied immediately. Further sizes inside 250 ms are held, and the latest is applied after 250 ms with no new size. A repeat of the size just applied is ignored inside that window and accepted again after it.
2. Clamp. Dimensions are rounded down to even. Minimum is 64×64. H.264 (and any unknown codec id) is capped at 4096×4096. HEVC and AV1 are capped at 8192×8192. Over-max requests keep aspect ratio with integer math, then even-round. A result still below the minimum is rejected.
3. In window-capture mode, `SetWindowPos` sizes the captured HWND's client area to the clamped pixels. The call does not move, reorder, or activate the window, and it does not change the virtual display mode. A minimized window is left alone.
4. If the client rect does not land within 2 px (fixed-size window, exclusive fullscreen, no window bound), the encode size stays put and the frame is letterboxed. The log reason is `window-did-not-follow` or `window-minimized`.
5. If the client rect matches, capture is reinitialized. The Windows.Graphics.Capture frame pool is recreated when `ContentSize` changes. When the captured frame is within 2 px of the clamped size, the encoder is rebuilt at that size (NVENC, AMF, QSV, and software all go through this path). Bitrate scales from the original negotiated bitrate by pixel count, clamped to 500–500000 kbps. Mouse and touch mapping use the new encode size and the new capture size.
6. If the capture does not match within 800 ms, the encode size stays at the previous size and the new window is letterboxed (`capture-did-not-follow`).
7. A request equal to the size already being encoded still tries `SetWindowPos`. When the window already matches, the encoder is left alone (`already-encoding`).

A window that changes size on its own (no `0x5506`) reinitializes capture and letterboxes into the current encode size. It does not send `0x5507`.

## Logs

Each attempt logs one line:

```text
live-resize source=0x5506 requested=1920x1080 applied_window=1280x720 encode=1280x720 bitrate_kbps=8888 elapsed_ms=40
live-resize source=0x5506 requested=1920x1080 applied_window=1920x1080 encode=1920x1080 letterbox=1 elapsed_ms=2 reason=window-did-not-follow
live-resize source=0x5506 requested=1280x720 applied_window=1280x720 encode=1920x1080 letterbox=1 elapsed_ms=810 reason=capture-did-not-follow
```

`source` is `0x5506` or `resize.txt`. `elapsed_ms` is the time from the start of the window resize to the log line.

## Exercise it on the gaming PC

`tools/resize_request.py` prints the plaintext and can write the harness file.

```bash
python3 tools/resize_request.py --self-test
python3 tools/resize_request.py --width 1280 --height 720
python3 tools/resize_request.py --width 1280 --height 720 --write
```

`--write` creates `%ProgramData%\Rig\resize.txt` (or `RIG_DATA_DIR`, or `--dir`):

```text
width=1280
height=720
```

While a session is `RUNNING` and has a control peer, the host reads that file once, deletes it, and runs the same path as `0x5506`. This opts the session into `0x5507`. Do not use it against stock Moonlight.

Unit tests cover clamping, bitrate scaling, debounce, both packets, `resize.txt` parsing, and the `/serverinfo` field. They live in `tests/unit/test_live_resize.cpp`. The Windows GitHub Actions workflow builds with tests off, so that job checks that the host compiles; it does not run these tests.

## Rig

Build the 16-byte plaintext above and send it on the encrypted control stream. Read `<ClientResolutionChange>` from `/serverinfo` first. On `0x5507`, resize the decoder to the uint32 width and height, then wait for the IDR. Requests can be sent as the Mac window changes size; the host coalesces a burst.
