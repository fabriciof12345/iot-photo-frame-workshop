# AWS IoT Photo Frame Workshop

This repository contains the firmware and AWS infrastructure for a cloud-connected photo frame built with the [Waveshare ESP32-S3-Touch-LCD-7](https://www.waveshare.com/esp32-s3-touch-lcd-7.htm).

Upload a JPEG or PNG to a private S3 bucket, and the frame receives an AWS IoT Core notification, downloads the processed 800×480 RGB565 image, displays it, and saves it locally across restarts.

## Repository contents

- `main/` — ESP32-S3 firmware and bundled fallback frame
- `lambda/image_processor/` — authored Lambda source
- `infra/` — CloudFormation templates and prebuilt Lambda deployment ZIP
- `tools/` — Wi-Fi and fallback-image helpers

The console walkthrough is published separately from this code repository.

## Requirements

- Waveshare ESP32-S3-Touch-LCD-7
- A data-capable USB cable connected to the board's **UART1** port
- ESP-IDF 5.3.5
- A 2.4 GHz WPA2/WPA3 personal Wi-Fi network
- AWS IoT device certificate, private key, and Amazon Root CA 1

## Configure the firmware

Create the local certificate directory and copy the three device files:

```bash
mkdir -p main/certs
cp "<PATH_TO_DEVICE_CERTIFICATE>" main/certs/device-certificate.pem.crt
cp "<PATH_TO_PRIVATE_KEY>" main/certs/device-private.key
cp "<PATH_TO_AMAZON_ROOT_CA_1>" main/certs/AmazonRootCA1.pem
chmod 600 main/certs/*
```

Set your AWS IoT values in `main/network.c`:

```c
#define THING_NAME "<DEVICE_NAME>"
#define IOT_ENDPOINT "<IOT_ENDPOINT>"
```

Configure Wi-Fi without placing the password in shell history:

```bash
python3 tools/configure_wifi.py
```

Activate ESP-IDF, select the ESP32-S3 target, build, and flash:

```bash
. "$HOME/.espressif/tools/activate_idf_v5.3.5.sh"
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

Press **Control + ]** to close the serial monitor.

## Lambda processor

The ready-to-upload Python 3.12 x86_64 deployment package is included at:

```text
infra/dist/image-processor.zip
```

The authored source remains under `lambda/image_processor/`. Pixel art is disabled by default and can be enabled with `PIXEL_ART_ENABLED=true` or the matching CloudFormation parameter.

### Rebuild the package

Rebuild only after changing `app.py`, `pixel_art.py`, or `requirements.txt`:

```bash
rm -rf infra/dist/package infra/dist/image-processor.zip
mkdir -p infra/dist/package

python3 -m pip install \
  --platform manylinux2014_x86_64 \
  --implementation cp \
  --python-version 3.12 \
  --abi cp312 \
  --only-binary=:all: \
  --target infra/dist/package \
  -r lambda/image_processor/requirements.txt

cp lambda/image_processor/app.py infra/dist/package/
cp lambda/image_processor/pixel_art.py infra/dist/package/
(cd infra/dist/package && zip -qr ../image-processor.zip .)
(cd infra/dist && shasum -a 256 image-processor.zip > image-processor.sha256)
```

## Infrastructure

- `infra/bootstrap.yaml` creates the artifact bucket used for CloudFormation deployment assets.
- `infra/template.yaml` defines the private photo bucket, Lambda function, IAM role, log group, IoT Thing, policy, and certificate attachments.

The templates are optional; the same resources can be created manually in the AWS Console.

## Replace the bundled fallback frame

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r tools/requirements.txt
python tools/image_to_rgb565_raw.py ~/Pictures/photo.jpg main/sze.rgb565
```

## Security

The repository intentionally excludes `main/certs/`, `secrets/`, `sdkconfig`, and `main/wifi_config.h`. Never commit device private keys or Wi-Fi credentials.
