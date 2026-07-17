import queue
import threading

from config.settings import global_settings
from service_manager import ServiceManager
from tools.logger import logger


class AudioSendThread(threading.Thread):
    def __init__(self, sevice_manager: ServiceManager):
        super().__init__(daemon=True)
        self.sevice_manager = sevice_manager

    def run(self):
        remain_data = b""
        while not self.sevice_manager.stop_event.is_set():
            try:
                audio_data = self.sevice_manager.audio_queue.get(timeout=1)
                if self.sevice_manager.get_active_tts_request() is None:
                    remain_data = b""
                    continue

                if isinstance(audio_data, bytes):
                    samples_per_frame = int(
                        self.sevice_manager.audio_processor.frame_duration_ms
                        * self.sevice_manager.audio_processor.sample_rate
                        / 1000
                    ) * 2
                    audio_data = remain_data + audio_data
                    for i in range(0, len(audio_data), samples_per_frame):
                        if self.sevice_manager.get_active_tts_request() is None:
                            remain_data = b""
                            break

                        frame_slice = audio_data[i:i + samples_per_frame]
                        if len(frame_slice) == samples_per_frame:
                            opus_data = self.sevice_manager.audio_processor.encode_audio(frame_slice)
                            bin_data = self.sevice_manager.audio_processor.pack_bin_frame(
                                type=0,
                                version=global_settings.protocol_version,
                                payload=opus_data,
                            )
                            self.sevice_manager.ws_send_queue.put(bin_data)
                        else:
                            remain_data = frame_slice
            except queue.Empty:
                continue
            except Exception as e:
                logger.error(f"TTS发送线程发生错误: {e}")
