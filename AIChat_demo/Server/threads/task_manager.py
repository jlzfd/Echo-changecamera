from concurrent.futures import ThreadPoolExecutor
from tools.logger import logger


class TaskManager:
    def __init__(self, max_workers=5):
        self.executor = ThreadPoolExecutor(max_workers=max_workers)
        self.futures = set()

    def submit_task(self, func, *args, **kwargs):
        future = self.executor.submit(func, *args, **kwargs)
        self.futures.add(future)
        future.add_done_callback(self._on_done)
        logger.info(f"Task submitted: {func.__name__}, args={args}, kwargs={kwargs}")
        return future

    def _on_done(self, future):
        self.futures.discard(future)
        try:
            future.result()
        except Exception as exc:
            logger.error(f"Background task failed: {exc}")

    def shutdown(self):
        self.executor.shutdown(wait=False, cancel_futures=True)
