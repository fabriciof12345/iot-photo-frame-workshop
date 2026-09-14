"""S3-triggered image processor for the AWS IoT photo frame."""

from __future__ import annotations

import hashlib
import json
import logging
import os
import re
from io import BytesIO
from typing import Any
from urllib.parse import unquote_plus

from PIL import Image, ImageOps

from pixel_art import PixelArtOptions, pixelate

LCD_WIDTH = 800
LCD_HEIGHT = 480
RGB565_SIZE = LCD_WIDTH * LCD_HEIGHT * 2

LOGGER = logging.getLogger()
LOGGER.setLevel(logging.INFO)

_S3_CLIENT = None
_IOT_CLIENT = None


def pixel_art_options_from_environment() -> PixelArtOptions | None:
    """Build optional pixel-art settings from Lambda environment variables."""
    enabled = os.environ.get("PIXEL_ART_ENABLED", "false").strip().lower()
    if enabled not in {"1", "true", "yes", "on"}:
        return None

    return PixelArtOptions(
        block_size=int(os.environ.get("PIXEL_ART_BLOCK_SIZE", "8")),
        palette_size=int(os.environ.get("PIXEL_ART_PALETTE_SIZE", "32")),
        dither=os.environ.get("PIXEL_ART_DITHER", "none").strip().lower(),
    )


def image_to_rgb565(source: bytes, pixel_options: PixelArtOptions | None = None) -> bytes:
    """Convert an image into an 800x480 little-endian RGB565 frame."""
    with Image.open(BytesIO(source)) as opened:
        image = ImageOps.exif_transpose(opened)
        if pixel_options is None:
            fitted = ImageOps.fit(
                image.convert("RGB"),
                (LCD_WIDTH, LCD_HEIGHT),
                method=Image.Resampling.LANCZOS,
            )
        else:
            fitted = pixelate(
                image,
                (LCD_WIDTH, LCD_HEIGHT),
                pixel_options,
            )

    output = bytearray(RGB565_SIZE)
    offset = 0
    for red, green, blue in fitted.getdata():
        value = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        output[offset] = value & 0xFF
        output[offset + 1] = value >> 8
        offset += 2

    return bytes(output)


def _clients():
    global _S3_CLIENT, _IOT_CLIENT
    if _S3_CLIENT is None or _IOT_CLIENT is None:
        import boto3

        _S3_CLIENT = boto3.client("s3")
        endpoint = os.environ["IOT_DATA_ENDPOINT"]
        _IOT_CLIENT = boto3.client("iot-data", endpoint_url=f"https://{endpoint}")
    return _S3_CLIENT, _IOT_CLIENT


def _safe_stem(key: str) -> str:
    filename = key.rsplit("/", 1)[-1]
    stem = filename.rsplit(".", 1)[0]
    sanitized = re.sub(r"[^A-Za-z0-9._-]+", "-", stem).strip("-._")
    return sanitized[:80] or "photo"


def lambda_handler(event: dict[str, Any], context: Any) -> dict[str, Any]:
    s3, iot = _clients()
    expected_bucket = os.environ["PHOTO_BUCKET"]
    topic = os.environ["IMAGE_READY_TOPIC"]
    expires_in = int(os.environ.get("PRESIGNED_URL_SECONDS", "3600"))
    results = []

    for record in event.get("Records", []):
        if record.get("eventSource") != "aws:s3":
            continue

        bucket = record["s3"]["bucket"]["name"]
        key = unquote_plus(record["s3"]["object"]["key"])
        if bucket != expected_bucket or not key.startswith("incoming/"):
            LOGGER.info("Skipping object s3://%s/%s", bucket, key)
            continue

        LOGGER.info("Processing s3://%s/%s", bucket, key)
        source = s3.get_object(Bucket=bucket, Key=key)["Body"].read()
        pixel_options = pixel_art_options_from_environment()
        transform = "pixel-art" if pixel_options is not None else "smooth"
        if pixel_options is not None:
            LOGGER.info(
                "Pixel art enabled: block_size=%d palette_size=%d dither=%s",
                pixel_options.block_size,
                pixel_options.palette_size,
                pixel_options.dither,
            )
        frame = image_to_rgb565(source, pixel_options)
        digest = hashlib.sha256(frame).hexdigest()

        request_id = getattr(context, "aws_request_id", "local")
        output_key = f"processed/{_safe_stem(key)}-{request_id[:12]}.rgb565"
        s3.put_object(
            Bucket=bucket,
            Key=output_key,
            Body=frame,
            ContentType="application/octet-stream",
            CacheControl="no-store",
            Metadata={
                "width": str(LCD_WIDTH),
                "height": str(LCD_HEIGHT),
                "format": "rgb565-le",
                "transform": transform,
                "sha256": digest,
                "source-key": key,
            },
        )

        url = s3.generate_presigned_url(
            "get_object",
            Params={"Bucket": bucket, "Key": output_key},
            ExpiresIn=expires_in,
        )
        message = {
            "version": request_id,
            "sourceKey": key,
            "key": output_key,
            "url": url,
            "format": "rgb565-le",
            "transform": transform,
            "width": LCD_WIDTH,
            "height": LCD_HEIGHT,
            "size": len(frame),
            "sha256": digest,
        }
        iot.publish(topic=topic, qos=1, payload=json.dumps(message).encode("utf-8"))
        LOGGER.info("Published image-ready notification for s3://%s/%s", bucket, output_key)
        results.append({
            "sourceKey": key,
            "outputKey": output_key,
            "sha256": digest,
            "transform": transform,
        })

    return {"processed": len(results), "results": results}
