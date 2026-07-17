import os

import dashscope


def _get_env_int(name, default):
    value = os.getenv(name)
    if value is None or value == "":
        return default
    try:
        return int(value)
    except ValueError:
        return default


class Settings:
    def __init__(self):
        self.protocol_version = _get_env_int("AICHAT_PROTOCOL_VERSION", 2)

        self.DASHSCOPE_API_KEY = os.getenv(
            "AICHAT_DASHSCOPE_API_KEY",
            "sk-d8e4dc07bc01425fa83e851bc1d66b7f",
        )
        dashscope.api_key = self.DASHSCOPE_API_KEY

        self.INTENT_MODEL = os.getenv("AICHAT_INTENT_MODEL", "qwen-turbo")
        self.CHAT_MODEL = os.getenv("AICHAT_CHAT_MODEL", "qwen-turbo")
        self.VISION_MODEL = os.getenv("AICHAT_VISION_MODEL", "qwen-vl-plus")

        self.ASR_DEVICE = os.getenv("AICHAT_ASR_DEVICE", "cpu")
        self.VAD_DEVICE = os.getenv("AICHAT_VAD_DEVICE", "cpu")

        self.API_TIMEOUT = _get_env_int("AICHAT_API_TIMEOUT_SEC", 10)
        self.VISION_CONTEXT_TTL_MS = _get_env_int("AICHAT_VISION_CONTEXT_TTL_MS", 15000)
        self.AUDIO_GATE_RMS_THRESHOLD = _get_env_int("AICHAT_AUDIO_GATE_RMS_THRESHOLD", 180)
        self.AUDIO_GATE_HANGOVER_FRAMES = _get_env_int("AICHAT_AUDIO_GATE_HANGOVER_FRAMES", 6)
        self.BARGE_IN_MIN_RMS = _get_env_int("AICHAT_BARGE_IN_MIN_RMS", 260)

    def Set_API_Key(self, aliyun_api_key):
        if not aliyun_api_key:
            return
        self.DASHSCOPE_API_KEY = aliyun_api_key
        dashscope.api_key = aliyun_api_key


global_settings = Settings()
