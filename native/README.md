# vgaudio.dll — звуковой движок VolumeGuard 2.0

Берёт звук, который играет устройство-источник (WASAPI loopback), обрабатывает и выводит на другое устройство.

```
один поток, опрос каждую ~1 мс (push mode, с 2.1):
захват (loopback) ──► FrameRing ──► DriftResampler.produce ──► Processor ──► очередь наушников
                      < 1 пакета     ±0,5 %, sinc               усиление → Game → лимитер 5 мс
                                          ▲
                                     OutputPacer: держит среднюю очередь = запас + (порция + пакет) / 2
```

| Файл | Что внутри |
|---|---|
| `include/vgaudio.h` | C API для VolumeGuard.exe (`vg_start`, `vg_stop`, `vg_set_params`, `vg_get_status`, `vg_list_devices`) |
| `src/Engine.cpp` | WASAPI: устройства, потоки, MMCSS «Pro Audio», переподключение, `audio.log` |
| `src/Bridge.hpp` | Кольцевой буфер, ресемплер с подстройкой под часы, преобразование форматов (float/16/24/32 бит, 1–8 каналов) |
| `src/Processor.hpp` | Цепочка обработки — та же DSP, что в плагине (`plugin/src/GameDsp.hpp`, `LimiterDsp.hpp`) |
| `tests/test_bridge.cpp` | 10 минут симуляции: дрейф ±1000 ppm, 44,1/48/96 кГц, дрожание пакетов, пауза 3 с; SNR > 80 дБ |
| `tests/test_processor.cpp` | Потолок во всех режимах, bypass, весь путь 5.1 int16 → stereo float |
| `tests/test_pump.cpp` | Push mode: пакеты 2,7/10 мс, дрейф, опоздания потока, пауза источника; пропуски и задержка |
| `tests/smoke.cpp` | Проверка DLL на Windows (CI) |

**Подстройка часов.** Ошибка буфера e (в секундах) подчиняется e' = дрейф − коррекция; ПИ-регулятор
(kP 0,54, kI 0,09) даёт собственную частоту 0,3 рад/с и затухание 0,9 — выход на режим за ~15 с без перерегулирования.
Измерение буфера учитывает время с последнего пакета, иначе пила от пакетов по 10 мс раскачивает регулятор.
Буфер начинается с 25 мс, растёт на 5 мс после пропуска из-за неровных пакетов и уменьшается на 1 мс за 30 с,
если запас больше 8 мс не понадобился. Пока игра молчит, движок играет тишину в источник, чтобы Windows
продолжала отдавать пакеты.

**Сборка:** `./build.sh` (MinGW-w64). Тесты:

```sh
g++ -O2 -std=c++17 -Isrc tests/test_bridge.cpp -o test_bridge && ./test_bridge
g++ -O2 -std=c++17 -Isrc -I../plugin/src tests/test_processor.cpp -o test_processor && ./test_processor
```
