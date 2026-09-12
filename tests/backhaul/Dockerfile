FROM python:3.13-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends g++ git libmbedtls-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
