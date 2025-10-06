from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
import asyncio
import socket
from typing import Set
from contextlib import asynccontextmanager
import logging
import uvicorn
import paho.mqtt.publish as publish

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

frame_buffer = bytearray()
connected_clients: Set[WebSocket] = set()
UDP_IP = "0.0.0.0"
UDP_PORT = 5000
MQTT_BROKER_IP = "localhost"
MQTT_BROKER_PORT = 1883

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((UDP_IP, UDP_PORT))
logger.info(f"UDP socket bound to {UDP_IP}:{UDP_PORT}")

def is_shutdown_disconnect(exc: Exception) -> bool:
    while exc:
        if isinstance(exc, asyncio.CancelledError):
            return True
        exc = exc.__cause__ or exc.__context__
    return False

@asynccontextmanager
async def lifespan(app: FastAPI):
    task = asyncio.create_task(udp_receiver())
    yield
    logger.info("Entering shutdown phase")
    task.cancel()
    logger.info("UDP receiver task cancelled")

app = FastAPI(lifespan=lifespan)

async def udp_receiver():
    global frame_buffer, connected_clients
    loop = asyncio.get_event_loop()
    logger.info("UDP receiver task started")
    try:
        while True:
            try:
                data, addr = await loop.run_in_executor(None, sock.recvfrom, 65507)
                frame_buffer.extend(data)
            except Exception as e:
                logger.error(f"Error during UDP socket read: {type(e).__name__}", exc_info=e)
                raise
            if len(frame_buffer) > 2 and frame_buffer[-2:] == b'\xFF\xD9':
                dead_clients = set()
                for client in connected_clients.copy():
                    try:
                        await client.send_bytes(frame_buffer)
                    except WebSocketDisconnect as e:
                        if is_shutdown_disconnect(e):
                            logger.info("WebSocket closed due to shutdown.")
                            raise
                        else:
                            logger.info(f"Client disconnected during send: {type(e).__name__}", exc_info=e)
                            dead_clients.add(client)
                for dc in dead_clients:
                    connected_clients.discard(dc)
                frame_buffer = bytearray()
    except Exception as e:
        logger.error(f"UDP receiver stopped: {e}", exc_info=e)
        raise
    finally:
        frame_buffer = bytearray()
        logger.info("UDP receiver exiting.")

@app.websocket("/ws/video_feed")
async def video_feed(websocket: WebSocket):
    global connected_clients
    await websocket.accept()
    connected_clients.add(websocket)
    logger.info(f"New WebSocket client connected. Total clients: {len(connected_clients)}")
    try:
        while True:
            await asyncio.sleep(0.1)
    except WebSocketDisconnect:
        connected_clients.remove(websocket)
        logger.info(f"WebSocket client disconnected. Total clients: {len(connected_clients)}")

@app.get("/", response_class=HTMLResponse)
async def root():
    return """
    <html>
        <head>
            <title>ESP32 CAM Stream</title>
        </head>
        <body>
            <h1>ESP32 CAM Stream</h1>
            <img id="video" width="320" height="240" />
            <p id="status">Waiting for video stream...</p>
            <div style="display: flex; align-items:center; gap: 8px; flex-direction: column;">
                <div>
                    <button onclick="sendCommand('esp32/cam/start')">Start Stream</button>
                    <button onclick="sendCommand('esp32/cam/stop')">Stop Stream</button>
                </div>
                <div>
                    <button onclick="sendCommand('esp32/motors/forward')">⬆️</button>
                </div>
                <div style="display: flex; align-items:center; gap: 8px; flex-direction: row;">
                    <button onclick="sendCommand('esp32/motors/left')">⬅️</button>
                    <button onclick="sendCommand('esp32/motors/backward')">⬇️</button>
                    <button onclick="sendCommand('esp32/motors/right')">➡️</button>
                </div>
            </div>
            <script>
                const statusElement = document.getElementById('status');
                const ws = new WebSocket("ws://" + window.location.host + "/ws/video_feed");
                ws.binaryType = "arraybuffer";
                ws.onopen = () => {
                    statusElement.textContent = "WebSocket connected!";
                    console.log("WebSocket connected");
                };
                ws.onmessage = (event) => {
                    const blob = new Blob([event.data], { type: "image/jpeg" });
                    document.getElementById("video").src = URL.createObjectURL(blob);
                    statusElement.textContent = "Streaming video...";
                };
                ws.onerror = (error) => {
                    statusElement.textContent = "WebSocket error: " + error.message;
                    console.error("WebSocket error:", error);
                };
                ws.onclose = () => {
                    statusElement.textContent = "WebSocket disconnected";
                    console.log("WebSocket disconnected");
                };

                function sendCommand(topic) {
                    fetch(`/publish?topic=${topic}`)
                        .then(response => response.text())
                        .then(data => console.log(data))
                        .catch(error => console.error('Error:', error));
                }
            </script>
        </body>
    </html>
    """

@app.get("/publish")
async def publish_command(topic: str):
    publish.single(topic, payload="", hostname=MQTT_BROKER_IP)
    return f"Published command to {topic}"

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000, lifespan="on")
