#!/usr/bin/env python3

import os
import re
import signal
import subprocess
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer

ENGINE = "/usr/libexec/sail-recorder/sail-recorder-engine"
PARECORD = "/usr/bin/parecord"
FFMPEG = "/usr/bin/ffmpeg"
PACTL = "/usr/bin/pactl"

AUDIO_SOURCE = "sink.deep_buffer.monitor"
VIDEO_DIR = os.path.join(os.path.expanduser("~"), "Videos")

VIDEO_TS = "/tmp/sail_recorder_video.ts"
AUDIO_WAV = "/tmp/sail_recorder_audio.wav"

ENGINE_LOG = "/tmp/sail_recorder_engine.log"
AUDIO_LOG = "/tmp/sail_recorder_parecord.log"
FFMPEG_LOG = "/tmp/sail_recorder_ffmpeg.log"
DAEMON_LOG = "/tmp/sail_recorder_daemon.log"
ROUTE_LOG = "/tmp/sail_recorder_route.log"

LAST_VIDEO = "/tmp/sail_recorder_last_video"

video_process = None
audio_process = None
engine_log_handle = None
audio_log_handle = None

audio_start_us = None
engine_start_us = None

filter_thread = None
filter_stop = threading.Event()
filter_lock = threading.RLock()
muted_inputs = {}
classification_seen = set()

PLAYBACK_TOKENS = (
    "music",
    "video",
    "movie",
    "game",
    "event",
    "notification",
    "alarm",
    "ringtone",
    "system",
    "playback",
    "media",
)

MIC_TOKENS = (
    "microphone",
    "mic-input",
    "mic_input",
    "capture",
    "recording",
    "recorder",
    "voice",
    "voip",
    "call",
    "communication",
    "phone",
    "ofono",
    "telepathy",
    "uplink",
    "hotword",
    "speech-recognition",
    "camcorder",
    "loopback",
    "sidetone",
)


def log(message):
    try:
        with open(DAEMON_LOG, "a", encoding="utf-8") as handle:
            handle.write(
                time.strftime("%Y-%m-%d %H:%M:%S ")
                + message
                + "\n"
            )
    except Exception:
        pass


def remove(path):
    try:
        os.remove(path)
    except FileNotFoundError:
        pass


def close_handles():
    global engine_log_handle
    global audio_log_handle

    for handle in (engine_log_handle, audio_log_handle):
        if handle:
            try:
                handle.flush()
                handle.close()
            except Exception:
                pass

    engine_log_handle = None
    audio_log_handle = None


def stop_process(process, name):
    if not process or process.poll() is not None:
        return

    try:
        process.send_signal(signal.SIGINT)
    except Exception:
        pass

    try:
        process.wait(timeout=5)
        log(f"{name} stopped with SIGINT")
        return
    except Exception:
        pass

    try:
        process.terminate()
        process.wait(timeout=2)
        log(f"{name} stopped with TERM")
        return
    except Exception:
        pass

    try:
        process.kill()
        process.wait(timeout=1)
        log(f"{name} stopped with KILL")
    except Exception as error:
        log(f"{name} stop error: {error}")



def route_log(message):
    try:
        with open(ROUTE_LOG, "a", encoding="utf-8") as handle:
            handle.write(
                time.strftime("%Y-%m-%d %H:%M:%S ")
                + message
                + "\n"
            )
    except Exception:
        pass


def pulse_environment():
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    environment["LANG"] = "C"

    runtime_dir = environment.get("XDG_RUNTIME_DIR")
    if not runtime_dir:
        runtime_dir = f"/run/user/{os.getuid()}"
        environment["XDG_RUNTIME_DIR"] = runtime_dir

    if not environment.get("PULSE_SERVER"):
        socket_path = os.path.join(runtime_dir, "pulse", "native")
        if os.path.exists(socket_path):
            environment["PULSE_SERVER"] = "unix:" + socket_path

    return environment


def pactl(arguments):
    process = subprocess.run(
        [PACTL] + list(arguments),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=pulse_environment(),
    )

    return process.returncode, process.stdout.strip()


def clean_property(value):
    value = value.strip()

    if (
        len(value) >= 2
        and value.startswith('"')
        and value.endswith('"')
    ):
        return value[1:-1]

    return value


def list_sink_inputs():
    return_code, output = pactl(["list", "sink-inputs"])

    if return_code != 0:
        raise RuntimeError("Oynatma akislari okunamadi: " + output)

    results = []
    current = None
    in_properties = False

    def append_current():
        if current is not None and current.get("index"):
            results.append(current.copy())

    for raw_line in output.splitlines():
        line = raw_line.strip()

        if line.startswith("Sink Input #"):
            append_current()
            current = {
                "index": line.split("#", 1)[1].strip(),
                "sink": None,
                "driver": "",
                "owner_module": "",
                "muted": False,
                "properties": {},
            }
            in_properties = False
            continue

        if current is None:
            continue

        if line == "Properties:":
            in_properties = True
            continue

        if line.startswith("Sink:"):
            current["sink"] = line.split(":", 1)[1].strip()
            in_properties = False
            continue

        if line.startswith("Driver:"):
            current["driver"] = line.split(":", 1)[1].strip()
            in_properties = False
            continue

        if line.startswith("Owner Module:"):
            current["owner_module"] = line.split(":", 1)[1].strip()
            in_properties = False
            continue

        if line.startswith("Mute:"):
            current["muted"] = (
                line.split(":", 1)[1].strip().lower() == "yes"
            )
            in_properties = False
            continue

        if in_properties and "=" in line:
            key, value = line.split("=", 1)
            current["properties"][key.strip()] = clean_property(value)

    append_current()
    return results


def stream_identity(item):
    properties = item.get("properties", {})

    return "|".join(
        [
            item.get("driver", ""),
            item.get("owner_module", ""),
            properties.get("application.name", ""),
            properties.get("application.process.binary", ""),
            properties.get("media.name", ""),
            properties.get("media.role", ""),
            properties.get("module-stream-restore.id", ""),
        ]
    )


def stream_description(item):
    properties = item.get("properties", {})

    return (
        f"id={item.get('index')} "
        f"sink={item.get('sink')} "
        f"driver={item.get('driver')} "
        f"owner={item.get('owner_module')} "
        f"app={properties.get('application.name', '')!r} "
        f"binary={properties.get('application.process.binary', '')!r} "
        f"media={properties.get('media.name', '')!r} "
        f"role={properties.get('media.role', '')!r} "
        f"restore={properties.get('module-stream-restore.id', '')!r}"
    )


def is_allowed_playback(item):
    # Only native client streams can be the separate microphone/playback
    # streams observed on this device. Internal PulseAudio streams are left
    # untouched.
    if item.get("driver", "") != "protocol-native.c":
        return True

    properties = item.get("properties", {})

    media_name = properties.get("media.name", "").lower()
    media_role = properties.get("media.role", "").lower()
    restore_id = properties.get("module-stream-restore.id", "").lower()
    application_name = properties.get("application.name", "").lower()
    process_binary = properties.get("application.process.binary", "").lower()

    searchable = " ".join(
        [
            application_name,
            process_binary,
            media_name,
            media_role,
            restore_id,
        ]
    )

    if any(token in searchable for token in MIC_TOKENS):
        return False

    # The verified Facebook/Instagram playback stream on this Xperia is
    # appsupportaudio + media.name=music. Sailfish UI sounds use event,
    # notification, alarm, ringtone or system labels.
    if any(token in media_name for token in PLAYBACK_TOKENS):
        return True

    if any(token in restore_id for token in PLAYBACK_TOKENS):
        return True

    if any(
        token in media_role
        for token in (
            "music",
            "video",
            "game",
            "event",
            "notification",
            "alarm",
            "ringtone",
            "system",
        )
    ):
        return True

    # Unknown protocol-native streams are blocked during screen recording.
    # This is deliberate: the previously observed microphone sidetone stream
    # had no reliable playback label, while real media had media.name=music.
    return False


def apply_stream_filter():
    global muted_inputs

    with filter_lock:
        current = list_sink_inputs()
        current_by_index = {
            str(item["index"]): item
            for item in current
        }

        # Remove stale entries when PulseAudio reuses an index for a different
        # client stream.
        for input_index in list(muted_inputs):
            item = current_by_index.get(input_index)
            saved = muted_inputs[input_index]

            if (
                item is None
                or stream_identity(item) != saved.get("identity", "")
            ):
                muted_inputs.pop(input_index, None)

        for item in current:
            if item.get("driver", "") != "protocol-native.c":
                continue

            input_index = str(item["index"])
            identity = stream_identity(item)
            allowed = is_allowed_playback(item)
            classification_key = identity + ("|ALLOW" if allowed else "|BLOCK")

            if classification_key not in classification_seen:
                classification_seen.add(classification_key)
                route_log(
                    ("ALLOW " if allowed else "BLOCK ")
                    + stream_description(item)
                )

            saved = muted_inputs.get(input_index)

            if allowed:
                # If this exact stream was muted by us before its metadata was
                # fully populated, release it immediately.
                if (
                    saved is not None
                    and saved.get("identity") == identity
                    and not saved.get("was_muted", False)
                ):
                    pactl(["set-sink-input-mute", input_index, "0"])
                    route_log("UNMUTED PLAYBACK " + stream_description(item))
                    muted_inputs.pop(input_index, None)

                continue

            if saved is None:
                muted_inputs[input_index] = {
                    "identity": identity,
                    "was_muted": bool(item.get("muted", False)),
                }

            if not item.get("muted", False):
                return_code, output = pactl(
                    ["set-sink-input-mute", input_index, "1"]
                )

                if return_code == 0:
                    route_log("MUTED " + stream_description(item))
                else:
                    route_log(
                        "MUTE FAILED "
                        + stream_description(item)
                        + " output="
                        + output
                    )


def filter_worker():
    while not filter_stop.wait(0.02):
        try:
            apply_stream_filter()
        except Exception as error:
            route_log("FILTER ERROR " + str(error))


def start_stream_filter():
    global filter_thread
    global classification_seen

    classification_seen = set()
    filter_stop.clear()
    apply_stream_filter()

    filter_thread = threading.Thread(
        target=filter_worker,
        name="sail-recorder-stream-filter",
        daemon=True,
    )
    filter_thread.start()
    route_log("FILTER STARTED mode=allow-labelled-playback")


def stop_stream_filter():
    global filter_thread
    global muted_inputs

    filter_stop.set()

    if filter_thread is not None:
        filter_thread.join(timeout=1.0)

    filter_thread = None

    with filter_lock:
        try:
            current_by_index = {
                str(item["index"]): item
                for item in list_sink_inputs()
            }

            for input_index, saved in muted_inputs.items():
                item = current_by_index.get(str(input_index))

                if item is None:
                    continue

                if stream_identity(item) != saved.get("identity", ""):
                    continue

                if not saved.get("was_muted", False):
                    pactl(["set-sink-input-mute", str(input_index), "0"])
                    route_log(
                        "RESTORED " + stream_description(item)
                    )
        except Exception as error:
            route_log("RESTORE ERROR " + str(error))

        muted_inputs = {}

    route_log("FILTER STOPPED")


def read_first_capture_us():
    try:
        content = open(
            ENGINE_LOG,
            "r",
            encoding="utf-8",
            errors="replace",
        ).read()

        match = re.search(
            r"RESULT_FIRST_CAPTURE_MONOTONIC_US\s+(\d+)",
            content,
        )

        if match:
            return int(match.group(1))
    except Exception as error:
        log(f"capture timestamp read error: {error}")

    return None


def start_recording():
    global video_process
    global audio_process
    global engine_log_handle
    global audio_log_handle
    global audio_start_us
    global engine_start_us

    video_running = (
        video_process is not None
        and video_process.poll() is None
    )

    audio_running = (
        audio_process is not None
        and audio_process.poll() is None
    )

    if video_running or audio_running:
        return "Kayit zaten devam ediyor"

    for path in (
        VIDEO_TS,
        AUDIO_WAV,
        ENGINE_LOG,
        AUDIO_LOG,
        FFMPEG_LOG,
        ROUTE_LOG,
        LAST_VIDEO,
    ):
        remove(path)

    os.makedirs(VIDEO_DIR, exist_ok=True)

    try:
        start_stream_filter()
    except Exception as error:
        route_log("FILTER START ERROR " + str(error))
        stop_stream_filter()
        return "Mikrofon akisi filtresi baslatilamadi"

    engine_log_handle = open(
        ENGINE_LOG,
        "w",
        encoding="utf-8",
    )

    audio_log_handle = open(
        AUDIO_LOG,
        "w",
        encoding="utf-8",
    )

    audio_start_us = time.monotonic_ns() // 1000

    audio_process = subprocess.Popen(
        [
            PARECORD,
            f"--device={AUDIO_SOURCE}",
            AUDIO_WAV,
        ],
        stdout=audio_log_handle,
        stderr=subprocess.STDOUT,
    )

    time.sleep(0.08)

    if audio_process.poll() is not None:
        close_handles()
        audio_process = None
        stop_stream_filter()
        return "Ses kaydi baslatilamadi"

    engine_start_us = time.monotonic_ns() // 1000

    video_process = subprocess.Popen(
        [
            ENGINE,
            VIDEO_TS,
            "120",
            "60",
            "50000000",
        ],
        stdout=engine_log_handle,
        stderr=subprocess.STDOUT,
    )

    time.sleep(0.25)

    if video_process.poll() is not None:
        stop_process(audio_process, "AUDIO")
        audio_process = None
        video_process = None
        close_handles()
        stop_stream_filter()

        return "Goruntu motoru baslatilamadi"

    log(
        f"START video_pid={video_process.pid} "
        f"audio_pid={audio_process.pid} "
        f"audio_start_us={audio_start_us}"
    )

    return "Sesli ekran kaydi basladi"


def stop_recording():
    global video_process
    global audio_process
    global audio_start_us
    global engine_start_us

    if (
        (video_process is None or video_process.poll() is not None)
        and
        (audio_process is None or audio_process.poll() is not None)
    ):
        stop_stream_filter()
        return "Aktif kayit yok"

    current_video = video_process
    current_audio = audio_process

    log("STOP requested")

    for process in (current_video, current_audio):
        if process and process.poll() is None:
            try:
                process.send_signal(signal.SIGINT)
            except Exception:
                pass

    stop_process(current_video, "VIDEO")
    stop_process(current_audio, "AUDIO")

    video_process = None
    audio_process = None

    close_handles()
    stop_stream_filter()
    time.sleep(0.3)

    if (
        not os.path.isfile(VIDEO_TS)
        or os.path.getsize(VIDEO_TS) < 1000
    ):
        return "Zaman bilgili goruntu dosyasi olusmadi"

    if (
        not os.path.isfile(AUDIO_WAV)
        or os.path.getsize(AUDIO_WAV) < 1000
    ):
        return "Sistem sesi dosyasi olusmadi"

    first_capture_us = read_first_capture_us()

    if (
        first_capture_us is not None
        and audio_start_us is not None
    ):
        audio_lead_seconds = max(
            0.0,
            (first_capture_us - audio_start_us) / 1000000.0,
        )
        lead_source = "first_capture"
    elif engine_start_us is not None and audio_start_us is not None:
        audio_lead_seconds = max(
            0.0,
            (engine_start_us - audio_start_us) / 1000000.0,
        )
        lead_source = "engine_start"
    else:
        audio_lead_seconds = 0.08
        lead_source = "fallback"

    output = os.path.join(
        VIDEO_DIR,
        "SailRecorder_"
        + datetime.now().strftime("%Y%m%d_%H%M%S")
        + ".mp4",
    )

    command = [
        FFMPEG,
        "-y",
        "-i", VIDEO_TS,
        "-ss", f"{audio_lead_seconds:.6f}",
        "-i", AUDIO_WAV,
        "-map", "0:v:0",
        "-map", "1:a:0",
        "-c:v", "copy",
        "-c:a", "aac",
        "-b:a", "192k",
        "-ar", "48000",
        "-ac", "2",
        "-af", "aresample=async=1:first_pts=0",
        "-shortest",
        "-avoid_negative_ts", "make_zero",
        "-movflags", "+faststart",
        output,
    ]

    with open(
        FFMPEG_LOG,
        "w",
        encoding="utf-8",
    ) as handle:
        result = subprocess.call(
            command,
            stdout=handle,
            stderr=handle,
        )

    if (
        result != 0
        or not os.path.isfile(output)
        or os.path.getsize(output) < 1000
    ):
        log(
            f"FFMPEG ERROR code={result} "
            f"lead={audio_lead_seconds:.6f}"
        )

        return "Video ve ses birlestirilemedi"

    try:
        os.chmod(output, 0o644)
    except Exception:
        pass

    with open(LAST_VIDEO, "w", encoding="utf-8") as handle:
        handle.write(output)

    log(
        f"SAVED {output} "
        f"lead={audio_lead_seconds:.6f} "
        f"lead_source={lead_source}"
    )

    audio_start_us = None
    engine_start_us = None

    return "Sesli video Galeri icin kaydedildi"


def status():
    video_running = (
        video_process is not None
        and video_process.poll() is None
    )

    audio_running = (
        audio_process is not None
        and audio_process.poll() is None
    )

    if video_running and audio_running:
        return "Kayit devam ediyor: goruntu ve ses aktif"

    if video_running:
        return "Uyari: goruntu aktif, ses durmus"

    if audio_running:
        return "Uyari: ses aktif, goruntu durmus"

    return "Hazir"


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            if self.path.startswith("/start"):
                message = start_recording()
            elif self.path.startswith("/stop"):
                message = stop_recording()
            elif self.path.startswith("/status"):
                message = status()
            else:
                message = "Bilinmeyen komut"
        except Exception as error:
            log(f"HTTP ERROR: {error}")
            message = f"Hata: {error}"

        data = message.encode("utf-8")

        self.send_response(200)
        self.send_header(
            "Content-Type",
            "text/plain; charset=utf-8",
        )
        self.send_header(
            "Content-Length",
            str(len(data)),
        )
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        return


log("SAIL RECORDER DAEMON START port=37822")
HTTPServer(("127.0.0.1", 37822), Handler).serve_forever()
