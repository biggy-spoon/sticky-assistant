# The sticky-assistant backend. See docker-compose.yml at the repository root,
# which runs this with a volume for everything worth keeping.
FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

# Everything that should outlive the container goes under /data: the
# database (history, settings, API keys entered on the device), plugins
# installed from the hub, and the downloaded speech model.
ENV DB_PATH=/data/sticky_assistant.db \
    PLUGINS_DIR=/data/plugins \
    HF_HOME=/data/models \
    PYTHONUNBUFFERED=1
VOLUME /data

EXPOSE 8000
HEALTHCHECK --interval=60s --timeout=5s --start-period=120s \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://localhost:8000/health', timeout=4)"
CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8000"]
