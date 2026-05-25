#!/bin/bash
# deploy_to_zimaboard.sh
# Push-button script to sync, configure, and boot the upgraded Xiaozhi server on your ZimaBoard.

set -e

ZIMA_IP="192.168.0.35"
ZIMA_USER="mo"
ZIMA_DIR="/home/mo/xiaozhi"

echo "=========================================================="
echo "🚀 UPGRADING XIAOZHI BACKEND ON ZIMABOARD ($ZIMA_IP)"
echo "=========================================================="

# 1. Create a temporary source folder locally to prepare sync
echo "📁 Preparing custom source files locally..."
LOCAL_SRC="./xiaozhi_custom_sync"
rm -rf "$LOCAL_SRC"
mkdir -p "$LOCAL_SRC/core/handle"
mkdir -p "$LOCAL_SRC/core/utils"
mkdir -p "$LOCAL_SRC/core/providers/tools/server_plugins"
mkdir -p "$LOCAL_SRC/core/providers/asr"
mkdir -p "$LOCAL_SRC/core/providers/tts"
mkdir -p "$LOCAL_SRC/plugins_func/functions"

# Copy connection handlers, executors, audio handlers, new providers & plugins into the sync tree
cp ./xiaozhi-esp32-server/main/xiaozhi-server/core/connection.py "$LOCAL_SRC/core/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/core/handle/sendAudioHandle.py "$LOCAL_SRC/core/handle/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/core/utils/util.py "$LOCAL_SRC/core/utils/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/core/providers/tools/server_plugins/plugin_executor.py "$LOCAL_SRC/core/providers/tools/server_plugins/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/core/providers/asr/base.py "$LOCAL_SRC/core/providers/asr/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/core/providers/asr/cloudflare.py "$LOCAL_SRC/core/providers/asr/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/core/providers/tts/polly.py "$LOCAL_SRC/core/providers/tts/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/plugins_func/functions/get_global_weather.py "$LOCAL_SRC/plugins_func/functions/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/plugins_func/functions/get_global_news.py "$LOCAL_SRC/plugins_func/functions/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/plugins_func/functions/change_role.py "$LOCAL_SRC/plugins_func/functions/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/plugins_func/functions/change_voice.py "$LOCAL_SRC/plugins_func/functions/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/plugins_func/functions/calculator.py "$LOCAL_SRC/plugins_func/functions/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/plugins_func/functions/system_status.py "$LOCAL_SRC/plugins_func/functions/"
cp ./xiaozhi-esp32-server/main/xiaozhi-server/plugins_func/functions/run_custom_routine.py "$LOCAL_SRC/plugins_func/functions/"

# 2. Sync files to ZimaBoard via SCP
echo "📡 Syncing files to ZimaBoard custom_src directory..."
ssh "$ZIMA_USER@$ZIMA_IP" "mkdir -p $ZIMA_DIR/custom_src/core $ZIMA_DIR/custom_src/core/handle $ZIMA_DIR/custom_src/core/utils $ZIMA_DIR/custom_src/core/providers/tools/server_plugins $ZIMA_DIR/custom_src/core/providers/asr $ZIMA_DIR/custom_src/core/providers/tts $ZIMA_DIR/custom_src/plugins_func/functions"

scp -r "$LOCAL_SRC"/* "$ZIMA_USER@$ZIMA_IP:$ZIMA_DIR/custom_src/"
rm -rf "$LOCAL_SRC"

# Copy upgraded config.yaml and agent-base-prompt.txt to ZimaBoard data directory
echo "📝 Copying upgraded config and prompt files to ZimaBoard..."
scp ./xiaozhi-esp32-server/main/xiaozhi-server/data/.config.yaml "$ZIMA_USER@$ZIMA_IP:$ZIMA_DIR/data/.config.yaml"
scp ./xiaozhi-esp32-server/main/xiaozhi-server/data/.agent-base-prompt.txt "$ZIMA_USER@$ZIMA_IP:$ZIMA_DIR/data/.agent-base-prompt.txt"

# 3. Apply upgrades to docker-compose.yml on ZimaBoard
echo "⚙️  Configuring docker-compose.yml and mapping AWS credentials..."
ssh "$ZIMA_USER@$ZIMA_IP" "bash -s" << 'EOF'
  set -e
  cd /home/mo/xiaozhi

  # Create a temporary docker-compose.yml with the new mounts and environment mappings
  cat << 'DOCKER' > docker-compose.yml
services:
  xiaozhi-esp32-server:
    image: ghcr.io/xinnan-tech/xiaozhi-esp32-server:server_latest
    container_name: xiaozhi-esp32-server
    restart: always
    security_opt:
    - seccomp:unconfined
    environment:
    - TZ=Asia/Shanghai
    - OLLAMA_API_KEY=${OLLAMA_API_KEY}
    - AWS_ACCESS_KEY_ID=${AWS_ACCESS_KEY_ID}
    - AWS_SECRET_ACCESS_KEY=${AWS_SECRET_ACCESS_KEY}
    - AWS_REGION_NAME=${AWS_REGION_NAME}
    ports:
    - 8000:8000
    - 8003:8003
    volumes:
    - ./data:/opt/xiaozhi-esp32-server/data:z
    - ./models/SenseVoiceSmall/model.pt:/opt/xiaozhi-esp32-server/models/SenseVoiceSmall/model.pt:z
    - ./custom_src/core/connection.py:/opt/xiaozhi-esp32-server/core/connection.py:z
    - ./custom_src/core/handle/sendAudioHandle.py:/opt/xiaozhi-esp32-server/core/handle/sendAudioHandle.py:z
    - ./custom_src/core/utils/util.py:/opt/xiaozhi-esp32-server/core/utils/util.py:z
    - ./custom_src/core/providers/tools/server_plugins/plugin_executor.py:/opt/xiaozhi-esp32-server/core/providers/tools/server_plugins/plugin_executor.py:z
    - ./custom_src/core/providers/asr/base.py:/opt/xiaozhi-esp32-server/core/providers/asr/base.py:z
    - ./custom_src/core/providers/asr/cloudflare.py:/opt/xiaozhi-esp32-server/core/providers/asr/cloudflare.py:z
    - ./custom_src/core/providers/tts/polly.py:/opt/xiaozhi-esp32-server/core/providers/tts/polly.py:z
    - ./custom_src/plugins_func/functions/get_global_weather.py:/opt/xiaozhi-esp32-server/plugins_func/functions/get_global_weather.py:z
    - ./custom_src/plugins_func/functions/get_global_news.py:/opt/xiaozhi-esp32-server/plugins_func/functions/get_global_news.py:z
    - ./custom_src/plugins_func/functions/change_role.py:/opt/xiaozhi-esp32-server/plugins_func/functions/change_role.py:z
    - ./custom_src/plugins_func/functions/change_voice.py:/opt/xiaozhi-esp32-server/plugins_func/functions/change_voice.py:z
    - ./custom_src/plugins_func/functions/calculator.py:/opt/xiaozhi-esp32-server/plugins_func/functions/calculator.py:z
    - ./custom_src/plugins_func/functions/system_status.py:/opt/xiaozhi-esp32-server/plugins_func/functions/system_status.py:z
    - ./custom_src/plugins_func/functions/run_custom_routine.py:/opt/xiaozhi-esp32-server/plugins_func/functions/run_custom_routine.py:z
  litellm:
    image: ghcr.io/berriai/litellm:main-stable
    container_name: litellm
    restart: unless-stopped
    environment:
    - AWS_ACCESS_KEY_ID=${AWS_ACCESS_KEY_ID}
    - AWS_SECRET_ACCESS_KEY=${AWS_SECRET_ACCESS_KEY}
    - AWS_REGION_NAME=${AWS_REGION_NAME}
    command:
    - --model
    - bedrock/converse/au.anthropic.claude-haiku-4-5-20251001-v1:0
    - --host
    - 0.0.0.0
    - --port
    - '4000'
DOCKER

  echo "🔄 Restarting Docker Compose Stack..."
  docker compose down
  docker compose up -d

  echo "⏳ Waiting for container boot..."
  sleep 3

  echo "📦 Installing boto3 and psutil dependencies inside the server container..."
  docker exec -t xiaozhi-esp32-server pip install boto3 psutil -i https://pypi.org/simple
EOF

echo "=========================================================="
echo "🎉 SUCCESS: ZimaBoard backend upgraded successfully!"
echo "=========================================================="
