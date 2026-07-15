# Joystick Bluetooth ESP32 - FSE

Projeto final da disciplina Fundamentos de Sistemas Embarcados. O sistema reproduz um controle/joystick sem fio usando uma ESP32, entradas fisicas e atuadores simples, implementado com ESP-IDF e FreeRTOS.

## Funcionalidades

- Leitura de joystick analogico em dois eixos.
- Leitura de quatro botoes digitais com pull-up interno.
- Comunicacao Bluetooth Low Energy HID, reconhecida pelo Windows como dispositivo de entrada.
- Envio do joystick como gamepad HID com eixos X/Y absolutos.
- Quatro botoes enviados como botoes 0 a 3 do gamepad.
- LED de status e buzzer ativo como atuadores locais.
- Coordenacao das operacoes com tasks e fila FreeRTOS.

## Hardware

| Funcao | GPIO ESP32 |
| --- | --- |
| Joystick eixo X | GPIO34 / ADC1_CH6 |
| Joystick eixo Y | GPIO35 / ADC1_CH7 |
| Botao 1 | GPIO27 |
| Botao 2 | GPIO26 |
| Botao 3 | GPIO25 |
| Botao 4 | GPIO33 |
| LED de status | GPIO2 |
| Buzzer ativo | GPIO18 |

Os botoes devem fechar o GPIO contra GND quando pressionados. O firmware habilita pull-up interno.

## Arquitetura FreeRTOS

O firmware foi dividido em tres tarefas principais:

| Task | Responsabilidade |
| --- | --- |
| `input_task` | Calibra o centro do joystick, le ADC/botoes, aplica deadzone e publica o estado em uma fila. |
| `hid_report_task` | Consome a fila e envia relatorios HID de mouse via BLE. |
| `output_task` | Controla LED e buzzer conforme conexao Bluetooth, botoes e comandos remotos. |

A fila `input_queue` separa a aquisicao das entradas da comunicacao BLE HID. O BLE tambem possui callbacks para conexao, desconexao e pareamento.

## Protocolo Bluetooth

Nome anunciado: `ESP32Gamepad`

O firmware usa o perfil HID BLE oficial do ESP-IDF. No Windows, abra Bluetooth > Adicionar dispositivo > Bluetooth e conecte em `ESP32Gamepad`.

Mapeamento HID:

| Entrada fisica | Acao no host |
| --- | --- |
| Eixo X/Y do joystick | `axes[0]` e `axes[1]` |
| Botao 1 | `buttons[0]` |
| Botao 2 | `buttons[1]` |
| Botao 3 | `buttons[2]` |
| Botao 4 | `buttons[3]` |

## Como compilar e gravar

### Com ESP-IDF

1. Instale e carregue o ambiente do ESP-IDF.
2. Configure o alvo:

```bash
idf.py set-target esp32
```

3. Compile:

```bash
idf.py build
```

4. Grave na placa:

```bash
idf.py -p COM3 flash monitor
```

Troque `COM3` pela porta serial da sua placa, se necessario.

### Com PlatformIO usando ESP-IDF

O `platformio.ini` ja esta configurado com `framework = espidf`.

```bash
pio run
pio run -t upload
pio device monitor
```

## Demonstracao em video

Adicionar aqui o link do video de demonstracao com ate 10 minutos, mostrando:

- leitura do joystick;
- acionamento dos botoes;
- LED e buzzer respondendo;
- conexao Bluetooth;
- pareamento Bluetooth no Windows;
- leitura pelo jogo via `navigator.getGamepads()`.
