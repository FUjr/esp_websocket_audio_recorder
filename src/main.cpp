#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#define I2S_SD 2
#define I2S_SCK 4
#define I2S_WS 5

struct AudioChain
{
  int16_t audioMetadata;
  struct AudioChain *next;
  struct AudioChain *prev;
};

class Audio
{
public:
  Audio();
  ~Audio();
  SemaphoreHandle_t audio_length_counter = xSemaphoreCreateMutex();
  AudioChain *audioChain;
  int16_t read();
  void write(int16_t metadata);
  int metadata_count;
  AudioChain *readPointer;
  AudioChain *writePointer;
};

Audio::Audio()
{
  audioChain = (AudioChain *)malloc(sizeof(AudioChain));
  audioChain->audioMetadata = 0;
  audioChain->next = NULL;
  audioChain->prev = audioChain;
  this->audioChain = audioChain;
  this->metadata_count = 0;
  this->readPointer = audioChain;
  this->writePointer = audioChain;
}

Audio::~Audio()
{
  AudioChain *current = audioChain;
  while (current->next != NULL)
  {
    AudioChain *temp = current;
    current = current->next;
    free(temp);
  }
  free(current);
  vSemaphoreDelete(this->audio_length_counter);
}

int16_t Audio::read()
{
  if (this->readPointer != NULL)
  {
    AudioChain *temp = this->readPointer;
    this->readPointer = this->readPointer->next;
    if (this->readPointer != NULL)
    {
      this->readPointer->prev = NULL;
    }
    // aquire lock
    xSemaphoreTake(this->audio_length_counter, portMAX_DELAY);
    this->metadata_count--;
    xSemaphoreGive(this->audio_length_counter);
    free(temp);
    if (this->readPointer != NULL)
    {
      return this->readPointer->audioMetadata;
    }
  }
  return -1;
}
void Audio::write(int16_t metadata)
{
  AudioChain *newChain = (AudioChain *)malloc(sizeof(AudioChain));
  newChain->audioMetadata = metadata;
  newChain->next = NULL;
  newChain->prev = this->writePointer;
  this->writePointer->next = newChain;
  this->writePointer = newChain;
  // aquire lock
  xSemaphoreTake(this->audio_length_counter, portMAX_DELAY);
  this->metadata_count++;
  xSemaphoreGive(this->audio_length_counter);
}

TaskHandle_t background_record_task;

static AudioChain *audioChain;
static int16_t audio_length = 0;
static Audio audio = Audio();
void background_record(void *pvParameters)
{
  while (true)
  {
    size_t bytesRead = 0;
    uint8_t buffer32[8 * 4] = {0};
    i2s_read(I2S_NUM_0, &buffer32, sizeof(buffer32), &bytesRead, 100);
    int samplesRead = bytesRead / 4;
    int16_t buffer16[8] = {0};
    for (int i = 0; i < samplesRead; i++)
    {
      uint8_t mid = buffer32[i * 4 + 2];
      uint8_t msb = buffer32[i * 4 + 3];
      uint16_t raw = (((uint32_t)msb) << 8) + ((uint32_t)mid);
      memcpy(&buffer16[i], &raw, sizeof(raw));
      audio.write(buffer16[i]);
    }
  }
}

const char *ssid = "D330_iot";
const char *password = "12345678a";

AsyncWebServer server(80); // 声明WebServer对象

AsyncWebSocket ws("/"); // WebSocket对象，url为/

// WebSocket事件回调函数
void onEventHandle(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT) // 有客户端建立连接
  {
    //Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
    //client->printf("Hello Client %u !", client->id()); // 向客户端发送数据
    //client->ping();                                    // 向客户端发送ping
  }
  else if (type == WS_EVT_DISCONNECT) // 有客户端断开连接
  {
    //Serial.printf("ws[%s][%u] disconnect: %u\n", server->url(), client->id());
    ws.cleanupClients(); // 关闭过多的WebSocket连接以节省资源s
  }
  else if (type == WS_EVT_ERROR) // 发生错误
  {
    // Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t *)arg), (char *)data);
  }
  else if (type == WS_EVT_PONG) // 收到客户端对服务器发出的ping进行应答（pong消息）
  {
    Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char *)data : "");
  }
  else if (type == WS_EVT_DATA) // 收到来自客户端的数据
  {
    // AwsFrameInfo *info = (AwsFrameInfo *)arg;
    // Serial.printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT) ? "text" : "binary", info->index, info->index + len);
    // data[len] = 0;
    // Serial.printf("%s\n", (char *)data);
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println();
  i2s_config_t i2s_config = {
      .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = 16000,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 64,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0};

  i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_SCK,
      .ws_io_num = I2S_WS,
      .data_out_num = -1,
      .data_in_num = I2S_SD};
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_start(I2S_NUM_0);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);
  SPIFFS.begin();
  String indexhtml =  SPIFFS.open("/index.html", "r").readString();
  
  
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected");
  Serial.print("IP Address:");
  Serial.println(WiFi.localIP());
  xTaskCreate(background_record, "background_record", 8192, NULL, 1, &background_record_task);
  ws.onEvent(onEventHandle); // 绑定回调函数
  server.addHandler(&ws);    // 将WebSocket添加到服务器中

  server.on("/", HTTP_GET, [indexhtml](AsyncWebServerRequest *request) { // 注册链接"/lambda"与对应回调函数（匿名函数形式声明）
    request->send(200, "text/html", indexhtml);                 // 向客户端发送响应和内容
  });

  server.begin(); // 启动服务器

  Serial.println("Web server started");
}
int read_count = 0;
int count = 0;
void loop()
{
  // delay(2000);
  // ws.textAll("lalala~~~"); // 向所有建立连接的客户端发送数据
  ws.cleanupClients(); // 关闭过多的WebSocket连接以节省资源
  if (audio.metadata_count > 1536)
  {
    int16_t buffer[1536];
    for (int i = 0; i < 1536; i++)
    {
      buffer[i] = audio.read();
    }

    ws.binaryAll((char *)&buffer, sizeof(buffer));
    read_count++;
  }
  else
  {
    delay(50);
    count++;
    if (count % 10 == 0)
    {
      int free_mem = xPortGetFreeHeapSize();
      printf("Free memory: %d\n", free_mem);
    }
  }
}
