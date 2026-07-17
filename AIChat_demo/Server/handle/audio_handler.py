import json

import numpy as np

from config.settings import global_settings
from service_manager import ServiceManager
from tools.logger import logger


class AudioHandler:
    def __init__(self, service_manager: ServiceManager):
        self.service_manager = service_manager
        self.service_manager.is_vad = False
        self._pre_speech_silence_frames = 0

    @staticmethod
    def _frame_rms(audio_data_np_array):
        if audio_data_np_array.size == 0:
            return 0.0
        return float(np.sqrt(np.mean(np.square(audio_data_np_array.astype(np.float32)))))

    async def handle_audio_message(self, msg):
        bin_protocol = self.service_manager.audio_processor.unpack_bin_frame(msg)
        if not bin_protocol:
            return

        protocol_version, frame_type, payload = bin_protocol
        if frame_type != 0 or protocol_version != global_settings.protocol_version or self.service_manager.is_vad:
            return

        pcm_data = self.service_manager.audio_processor.decode_audio(payload)
        audio_data_np_array = np.frombuffer(pcm_data, dtype=np.int16)
        frame_rms = self._frame_rms(audio_data_np_array)

        if frame_rms >= global_settings.BARGE_IN_MIN_RMS and self.service_manager.get_active_tts_request():
            self.service_manager.interrupt_active_tts("barge_in_voice")

        if not self.service_manager.has_active_speech:
            if frame_rms < global_settings.AUDIO_GATE_RMS_THRESHOLD:
                self._pre_speech_silence_frames += 1
                if self._pre_speech_silence_frames <= global_settings.AUDIO_GATE_HANGOVER_FRAMES:
                    return
            else:
                self.service_manager.has_active_speech = True
                self._pre_speech_silence_frames = 0

        vad_result = self.service_manager.vad_service.process_audio_frame(audio_data_np_array)
        if vad_result == 0:
            self.service_manager.asr_service.asr_add_audio_buffer(pcm_data)
            return

        self.service_manager.is_vad = True
        self.service_manager.has_active_speech = False
        self._pre_speech_silence_frames = 0

        if vad_result == 1:
            asr_res = self.service_manager.asr_service.asr_generate_text()
            self.service_manager.task_manager.submit_task(self.service_manager.chat_start_task, asr_res)
            res = {
                "type": "asr",
                "text": asr_res,
            }
            logger.info(f"asr result: {asr_res}")
            self.service_manager.ws_send_queue.put(json.dumps(res))
        elif vad_result == 2:
            res = {
                "type": "vad",
                "state": "no_speech",
            }
            logger.warning("vad result: no_speech")
            self.service_manager.ws_send_queue.put(json.dumps(res))
        elif vad_result == 3:
            asr_res = self.service_manager.asr_service.asr_generate_text()
            self.service_manager.task_manager.submit_task(self.service_manager.chat_start_task, asr_res)
            res = {
                "type": "asr",
                "text": asr_res,
            }
            logger.warning(f"vad result: buffer_full, asr result: {asr_res}")
            self.service_manager.ws_send_queue.put(json.dumps(res))
