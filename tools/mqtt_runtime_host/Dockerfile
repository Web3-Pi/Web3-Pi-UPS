# Isolated Linux host checks for machines whose native ASan runtime is unusable.
# No firmware flashing, broker access or runtime credentials are needed.
FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev patch \
    && rm -rf /var/lib/apt/lists/*
ENV PYTHONDONTWRITEBYTECODE=1 MQTT_TEST_SANITIZERS=address,undefined
WORKDIR /workspace
