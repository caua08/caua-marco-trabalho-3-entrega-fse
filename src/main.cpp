#include <Arduino.h>
#include <BleGamepad.h>

BleGamepad bleGamepad("Joystick ESP32 Gamepad V2", "ESP32", 100);

// Entradas analogicas do modulo joystick
const int PIN_X = 34;
const int PIN_Y = 35;

// Os quatro botoes fecham o GPIO contra o GND quando pressionados.
// Por isso eles sao configurados com INPUT_PULLUP no setup().
const int BUTTON_COUNT = 4;
const int BUTTON_PINS[BUTTON_COUNT] = {27, 26, 25, 33};
const int GAMEPAD_BUTTONS[BUTTON_COUNT] = {BUTTON_1, BUTTON_2, BUTTON_3, BUTTON_4};

// Zona morta em torno do centro do joystick para evitar tremedeira do analogico.
const int DEADZONE = 350;

// O ADC do ESP32 esta em 12 bits: as leituras analogicas vao de 0 a 4095.
const int ADC_MIN = 0;
const int ADC_MAX = 4095;

// A biblioteca de gamepad envia eixos no intervalo 0..32767.
// O centro do analogico fica em 16384.
const int AXIS_MIN = 0;
const int AXIS_CENTER = 16384;
const int AXIS_MAX = 32767;

// Guarda o ultimo estado lido para aplicar debounce simples nos botoes.
bool lastButtons[BUTTON_COUNT] = {HIGH, HIGH, HIGH, HIGH};

// Centro calibrado do joystick. Esses valores sao atualizados no setup().
int centerX = 2048;
int centerY = 2048;

// Faz uma media de varias leituras para reduzir ruido.
// Usamos isso para descobrir o centro real do joystick.
int readAverage(int pin) {
  long total = 0;

  for (int i = 0; i < 30; i++) {
    total += analogRead(pin);
    delay(5);
  }

  return total / 30;
}

// Garante que o valor enviado para o gamepad nunca saia do limite aceito.
int clampAxis(long value) {
  if (value < AXIS_MIN) {
    return AXIS_MIN;
  }

  if (value > AXIS_MAX) {
    return AXIS_MAX;
  }

  return value;
}

// Converte uma leitura analogica do ESP32 para um eixo de controle.
// Entrada: 0..4095 do ADC.
// Saida: 0..32767 para o Windows/jogos.
int axisToGamepad(int value, int center) {
  int delta = value - center;

  // Se o joystick estiver perto do centro, envia exatamente centro.
  if (abs(delta) < DEADZONE) {
    return AXIS_CENTER;
  }

  // Metade positiva do eixo: centro + deadzone ate o maximo do ADC.
  if (delta > 0) {
    return clampAxis(map(value, center + DEADZONE, ADC_MAX, AXIS_CENTER, AXIS_MAX));
  }

  // Metade negativa do eixo: minimo do ADC ate centro - deadzone.
  return clampAxis(map(value, ADC_MIN, center - DEADZONE, AXIS_MIN, AXIS_CENTER));
}

void setup() {
  // Serial e util para debug pelo monitor do PlatformIO.
  Serial.begin(115200);

  // Define o ADC em 12 bits, ou seja, analogRead() retorna 0..4095.
  analogReadResolution(12);

  // INPUT_PULLUP deixa o pino em HIGH quando solto.
  // Quando o botao fecha para GND, a leitura vira LOW.
  for (int i = 0; i < BUTTON_COUNT; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
  }

  // Da tempo para o circuito estabilizar antes de calibrar o centro.
  delay(500);
  centerX = readAverage(PIN_X);
  centerY = readAverage(PIN_Y);

  // Configura como o ESP32 se apresenta ao Windows via Bluetooth HID.
  BleGamepadConfiguration bleGamepadConfig;

  // Auto report desligado: atualizamos todos os estados e so depois enviamos.
  bleGamepadConfig.setAutoReport(false);

  // Faz o dispositivo se declarar como gamepad, nao mouse/teclado.
  bleGamepadConfig.setControllerType(CONTROLLER_TYPE_GAMEPAD);

  // VID/PID/revisao foram alterados para o Windows tratar como dispositivo novo
  // e nao reutilizar o cache antigo de quando o firmware era mouse.
  bleGamepadConfig.setVid(0x1234);
  bleGamepadConfig.setPid(0x0002);
  bleGamepadConfig.setGuidVersion(0x0002);

  // Mesmo intervalo usado por axisToGamepad().
  bleGamepadConfig.setAxesMin(AXIS_MIN);
  bleGamepadConfig.setAxesMax(AXIS_MAX);

  // Quatro botoes, sem D-pad/hat switch, e apenas eixos X/Y.
  bleGamepadConfig.setButtonCount(BUTTON_COUNT);
  bleGamepadConfig.setHatSwitchCount(0);
  bleGamepadConfig.setWhichAxes(true, true, false, false, false, false, false, false);

  // Inicia o servico BLE HID. A partir daqui o ESP32 pode ser pareado.
  bleGamepad.begin(&bleGamepadConfig);

  Serial.println("Joystick ESP32 Gamepad pronto para parear por Bluetooth.");
}

void loop() {
  // Se ainda nao houver conexao Bluetooth, nao vale enviar relatorios HID.
  if (!bleGamepad.isConnected()) {
    delay(100);
    return;
  }

  // Le os dois eixos fisicos do analogico.
  int x = analogRead(PIN_X);
  int y = analogRead(PIN_Y);

  // Converte as leituras cruas para coordenadas de gamepad.
  int gamepadX = axisToGamepad(x, centerX);
  int gamepadY = axisToGamepad(y, centerY);

  // Envia os valores como o analogico esquerdo do controle.
  bleGamepad.setLeftThumb(gamepadX, gamepadY);

  // Le e atualiza os quatro botoes.
  for (int i = 0; i < BUTTON_COUNT; i++) {
    bool button = digitalRead(BUTTON_PINS[i]);

    // Debounce simples: se mudou, espera 20 ms e confirma a leitura.
    if (button != lastButtons[i]) {
      delay(20);
      button = digitalRead(BUTTON_PINS[i]);
    }

    // Com INPUT_PULLUP, LOW significa pressionado.
    if (button == LOW) {
      bleGamepad.press(GAMEPAD_BUTTONS[i]);
    } else {
      bleGamepad.release(GAMEPAD_BUTTONS[i]);
    }

    lastButtons[i] = button;
  }

  // Envia um unico relatorio HID com eixos e botoes ja atualizados.
  bleGamepad.sendReport();

  // Taxa aproximada de 66 atualizacoes por segundo.
  delay(15);
}
