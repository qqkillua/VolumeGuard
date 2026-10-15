**VolumeGuard Limiter** — плагин-лимитер VST3 и CLAP для FL Studio, Ableton, Reaper и других программ с плагинами.

**Новое в 1.2.0 — игровой режим.** Кнопка **TACTICAL** делает выстрелы, взрывы и гул машин тише, а шаги громче:
в тестах разница между выстрелами и шагами сократилась с 37,6 до 22,7 дБ (при Amount 100 % — до 17,8 дБ).
**COMFORT** — мягкий вариант для фильмов и одиночных игр. Ручка **Amount** задаёт силу.
Направление звуков не меняется: оба канала обрабатываются одинаково.

Звук на выходе никогда не бывает громче потолка: плагин заглядывает на 5 мс вперёд и убавляет громкость заранее.

**Установка**
1. Скачайте `VolumeGuardLimiter-Windows.zip` ниже и до распаковки снимите блокировку: «Свойства» → «Разблокировать».
2. Папку `VST3\VolumeGuardLimiter.vst3` скопируйте в `C:\Program Files\Common Files\VST3\`.
3. Обновите список плагинов в своей программе и найдите **VolumeGuard Limiter**.

Параметры: Input Gain, Ceiling, Release, Bypass и индикатор Gain Reduction. Подробнее — в [plugin/README.md](https://github.com/qqkillua/VolumeGuard/blob/main/plugin/README.md).

Проверено [pluginval](https://github.com/Tracktion/pluginval) на максимальной строгости.
