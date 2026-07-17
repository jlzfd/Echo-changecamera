import requests

from config.settings import global_settings


class VisionService:
    def __init__(self, api_key):
        self.api_key = api_key
        self.url = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"

    def describe_image(self, base64_img, prompt=None):
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }
        payload = {
            "model": global_settings.VISION_MODEL,
            "input": {
                "messages": [
                    {
                        "role": "user",
                        "content": [
                            {"image": f"data:image/jpeg;base64,{base64_img}"},
                            {"text": prompt or "请用中文简洁描述当前画面。"},
                        ],
                    }
                ]
            },
        }
        response = requests.post(
            self.url,
            headers=headers,
            json=payload,
            timeout=global_settings.API_TIMEOUT,
        )
        if response.status_code != 200:
            raise Exception(f"Vision API error: {response.text}")

        result = response.json()
        return result["output"]["choices"][0]["message"]["content"][0]["text"]
