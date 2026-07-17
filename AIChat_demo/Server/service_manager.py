from services.vad_service import VADService
from services.asr_service import ASRService
from services.chat_service import ChatService
from services.tts_service import TTSService
from tools.registry import global_registry
from services.intent_service import IntentService
from tools.audio_processor import AudioProcessor
from threads.task_manager import TaskManager
from tools.logger import logger
from services.vision_service import VisionService
from config.settings import global_settings
import queue
import threading
import json
import time


class ServiceManager:
    def __init__(self):
        self.audio_processor = AudioProcessor()
        self.vad_service = VADService()
        self.asr_service = ASRService()
        self.intent_service = IntentService(global_registry)
        self.chat_service = ChatService()
        self.tts_service = TTSService()
        self.vision_service = VisionService(api_key=global_settings.DASHSCOPE_API_KEY)
        self.is_vad = False
        self.has_active_speech = False

        self.tts_text_queue = queue.Queue()
        self.audio_queue = queue.Queue()
        self.ws_send_queue = queue.Queue()

        self.stop_event = threading.Event()
        self.task_manager = TaskManager(max_workers=5)

        self._tts_context_lock = threading.Lock()
        self._active_tts_request_id = None
        self._vision_context_lock = threading.Lock()
        self._latest_vision_context = {
            "request_id": None,
            "source": None,
            "prompt": None,
            "text": None,
            "frame_seq": 0,
            "captured_ms": 0,
            "updated_ms": 0,
        }

        def continue_chat():
            return "继续聊天..."

        def dummy_function():
            return "视觉请求已触发"

        global_registry.register_function(
            function_name="look_at_environment",
            description="当需要观察用户周围环境时调用，例如用户问看到了什么，不需要参数",
            parameters={},
            impl=dummy_function,
        )

        def handle_exit_intent():
            return "再见"

        global_registry.register_function("continue_chat", "继续聊天意图", {}, continue_chat)
        global_registry.register_function("exit_chat", "结束对话意图", {}, handle_exit_intent)

    def set_active_tts_request(self, request_id):
        with self._tts_context_lock:
            self._active_tts_request_id = request_id

    def get_active_tts_request(self):
        with self._tts_context_lock:
            return self._active_tts_request_id

    def reset_services(self):
        self.is_vad = False
        self.has_active_speech = False
        self.vad_service.reset()
        self.asr_service.reset()
        self.chat_service.chat_clear()
        self.set_active_tts_request(None)
        try:
            self.tts_service.tts_close()
        except Exception:
            pass
        self._clear_queue(self.audio_queue)

    def _clear_queue(self, q):
        with q.mutex:
            q.queue.clear()

    def _tts_on_data(self, data):
        self.audio_queue.put(data)

    def _tts_on_complete(self):
        request_id = self.get_active_tts_request()
        msg = {
            "type": "tts",
            "state": "end",
        }
        if request_id:
            msg["request_id"] = request_id
        self.ws_send_queue.put(json.dumps(msg))

    def update_vision_context(self, request_id, source, prompt, text, frame_seq=0, captured_ms=0):
        with self._vision_context_lock:
            self._latest_vision_context = {
                "request_id": request_id,
                "source": source,
                "prompt": prompt,
                "text": text,
                "frame_seq": frame_seq,
                "captured_ms": captured_ms,
                "updated_ms": int(time.monotonic() * 1000),
            }

    def get_recent_vision_context(self):
        with self._vision_context_lock:
            ctx = dict(self._latest_vision_context)
        if not ctx.get("text"):
            return None
        now_ms = int(time.monotonic() * 1000)
        if global_settings.VISION_CONTEXT_TTL_MS > 0 and now_ms - ctx["updated_ms"] > global_settings.VISION_CONTEXT_TTL_MS:
            return None
        return ctx

    def build_chat_history_with_vision_context(self, text, history_list):
        ctx = self.get_recent_vision_context()
        if not ctx:
            return history_list

        lowered = text.lower()
        follow_up_markers = [
            "左边", "右边", "旁边", "那个", "它", "这个", "那边", "前面", "后面",
            "left", "right", "next to", "that one", "it", "beside",
        ]
        if not any(marker in lowered or marker in text for marker in follow_up_markers):
            return history_list

        augmented = list(history_list)
        augmented.append({
            "role": "system",
            "content": (
                "Recent visual context from the latest answered frame: "
                f"{ctx['text']} "
                f"(source={ctx['source']}, frame_seq={ctx['frame_seq']}, captured_ms={ctx['captured_ms']}). "
                "Use it only to resolve follow-up references if the user asks about left/right/that one/nearby objects."
            ),
        })
        return augmented

    def interrupt_active_tts(self, reason="barge_in"):
        request_id = self.get_active_tts_request()
        if request_id is None:
            return False

        logger.warning(f"Interrupt active TTS request_id={request_id}, reason={reason}")
        self._clear_queue(self.audio_queue)
        try:
            self.tts_service.tts_close()
        except Exception as exc:
            logger.warning(f"tts_close during interrupt failed: {exc}")

        msg = {
            "type": "tts",
            "state": "interrupted",
            "reason": reason,
            "request_id": request_id,
        }
        self.ws_send_queue.put(json.dumps(msg))
        self.set_active_tts_request(None)
        return True

    def chat_start_task(self, text):
        function_calls = self.intent_service.detect_intent(text)
        history_list = []

        for function_call in function_calls:
            if "function_call" not in function_call or "name" not in function_call["function_call"]:
                continue

            logger.info(f"Preparing function call: {function_call}")
            name = function_call["function_call"]["name"]
            if name == "continue_chat":
                pass
            elif name == "exit_chat":
                self.ws_send_queue.put(json.dumps({"type": "chat", "dialogue": "end"}))
            else:
                self.ws_send_queue.put(json.dumps(function_call))
                history_list.extend([
                    {"role": "user", "content": f"函数调用: {function_call}"},
                    {"role": "assistant", "content": "函数调用完成"},
                ])

        history_list = self.build_chat_history_with_vision_context(text, history_list)
        answers = self.chat_service.generate_chat_response(text, history=history_list, is_stream=True)
        if answers == -1:
            logger.error("LLM generation failed")
            return -1

        logger.info("Chat response streaming started")
        self.set_active_tts_request("chat-stream")
        for text_chunk in answers:
            print(text_chunk, end="", flush=True)
            self.tts_service.tts_speech_stream(text_chunk)
        print()
        self.tts_service.tts_close()
        if self.get_active_tts_request() == "chat-stream":
            self.set_active_tts_request(None)

    def vision_start_task(self, base64_img, request_id=None, prompt=None, source="unknown",
                          frame_seq=0, captured_ms=0):
        logger.info(f"Vision task started request_id={request_id}, source={source}")
        self.set_active_tts_request(request_id)

        try:
            answer = self.vision_service.describe_image(base64_img, prompt=prompt)
            logger.info(f"Vision result request_id={request_id}: {answer}")

            result_msg = {
                "type": "vision_result",
                "text": answer,
                "source": source,
                "prompt": prompt,
                "frame_seq": frame_seq,
                "captured_ms": captured_ms,
            }
            if request_id:
                result_msg["request_id"] = request_id
            self.ws_send_queue.put(json.dumps(result_msg))
            self.update_vision_context(request_id, source, prompt, answer, frame_seq, captured_ms)

            start_msg = {"type": "tts", "state": "start"}
            if request_id:
                start_msg["request_id"] = request_id
            self.ws_send_queue.put(json.dumps(start_msg))

            self.tts_service.tts_speech_stream(answer)
            self.tts_service.tts_close()

            end_msg = {"type": "chat", "dialogue": "end"}
            if request_id:
                end_msg["request_id"] = request_id
            self.ws_send_queue.put(json.dumps(end_msg))

            logger.info(f"Vision task completed request_id={request_id}")

        except Exception as e:
            logger.error(f"Vision task failed request_id={request_id}: {e}")
            error_msg = {"type": "error", "message": "视觉理解服务异常"}
            if request_id:
                error_msg["request_id"] = request_id
            self.ws_send_queue.put(json.dumps(error_msg))
        finally:
            if self.get_active_tts_request() == request_id:
                self.set_active_tts_request(None)
