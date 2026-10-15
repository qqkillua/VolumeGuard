**VolumeGuard Limiter** — плагин-лимитер VST3 и CLAP для FL Studio, Ableton, Reaper и других программ с плагинами.

**Новое в 1.1.0:** у плагина появилось своё окно — ползунки, Bypass и индикаторы входа, выхода и ослабления.
Раньше в FL Studio и некоторых других программах вместо него открывалось пустое окошко.

Звук на выходе никогда не бывает громче потолка: плагин заглядывает на 5 мс вперёд и убавляет громкость заранее.

**Установка**
1. Скачайте `VolumeGuardLimiter-Windows.zip` ниже и до распаковки снимите блокировку: «Свойства» → «Разблокировать».
2. Папку `VST3\VolumeGuardLimiter.vst3` скопируйте в `C:\Program Files\Common Files\VST3\`.
3. Обновите список плагинов в своей программе и найдите **VolumeGuard Limiter**.

Параметры: Input Gain, Ceiling, Release, Bypass и индикатор Gain Reduction. Подробнее — в [plugin/README.md](https://github.com/qqkillua/VolumeGuard/blob/main/plugin/README.md).

Проверено [pluginval](https://github.com/Tracktion/pluginval) на максимальной строгости.
