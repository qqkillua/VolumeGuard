<p align="center">
  <img src="assets/banner.jpg" alt="VolumeGuard — smart volume protection for Windows" width="100%">
</p>

<p align="center">
  <a href="https://github.com/qqkillua/VolumeGuard/releases/latest"><img src="https://img.shields.io/github/v/release/qqkillua/VolumeGuard?label=скачать&color=4c8dff" alt="Скачать"></a>
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011-0078d4" alt="Windows 10 | 11">
  <img src="https://img.shields.io/badge/.NET%20Framework-4.x-512bd4" alt=".NET Framework 4.x">
  <a href="LICENSE"><img src="https://img.shields.io/badge/лицензия-MIT-3ba55c" alt="MIT"></a>
</p>

**VolumeGuard** следит за громкостью каждого приложения и не даёт ему стать громче заданного уровня.
Реклама в браузере, взрыв в фильме или уведомление в Discord больше не оглушают: программа сама
плавно убавляет звук, а потом возвращает его обратно.

<p align="center">
  <img src="assets/screenshot.png" alt="Окно VolumeGuard" width="420">
</p>

## Возможности

- **Каждое приложение отдельно.** Все программы, которые выводят звук, показаны карточками со своей громкостью
  и кнопкой выключения звука. Ползунок тот же, что в микшере громкости Windows.
- **Лимит на приложение.** Включите «Лимит» и поставьте красную метку: приложение не будет громче этого уровня.
  Зелёная полоса показывает текущий уровень, красная — превышение.
- **Общий лимит устройства** для всего звука сразу.
- **Один переключатель** включает и выключает все ограничения; при выключении громкость сразу возвращается.
- **Выбор устройства вывода** или «По умолчанию» — тогда программа следует за устройством Windows.
- **Работает из трея**, запускается вместе с Windows, запоминает настройки каждой программы по имени.
- **Не спорит с вами:** если поменять громкость в микшере Windows или клавишами, программа примет это как новое значение.
- **Не оставляет звук тихим:** если программу закрыли аварийно, при следующем запуске она вернёт громкость, которую убавила.

## Установка

1. Скачайте `VolumeGuard.zip` со страницы [Releases](https://github.com/qqkillua/VolumeGuard/releases/latest).
2. **До распаковки** щёлкните по архиву правой кнопкой → «Свойства» → отметьте «Разблокировать» → «ОК».
3. Распакуйте и запустите `VolumeGuard.exe`. Установка не нужна.

Нужен .NET Framework 4.x — он уже есть в Windows 10 и 11.

> **Почему Windows предупреждает?** Программа не подписана платным сертификатом, а архив помечен как скачанный
> из интернета. Шаг 2 снимает эту пометку. Можно и не снимать: в окне SmartScreen нажмите «Подробнее» →
> «Выполнить в любом случае». Или соберите программу сами (см. ниже) — собранный на своём компьютере exe
> Windows не останавливает.

## Как это работает

VolumeGuard использует Windows Core Audio. У каждого приложения есть аудиосессия: `ISimpleAudioVolume`
управляет её громкостью, `IAudioMeterInformation` показывает пиковый уровень.

Каждые 50 мс программа читает пик, держит огибающую (мгновенная реакция на рост, плавный спад около 1,5 с)
и выставляет громкость `min(ваша громкость, лимит / уровень)`. После громкого звука громкость возвращается
плавно, за 2–3 секунды.

Лимитер работает через ползунок громкости, поэтому очень резкий звук может проскочить первые ~50 мс,
пока программа не среагировала.

Формулы, пример с цифрами и разбор «взрыва в фильме» — в [docs/limiter.md](docs/limiter.md).

## Сборка

Исходники в [`src/`](src). Достаточно компилятора, который уже есть в Windows:

```bat
build.cmd
```

Или вручную:

```bat
%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe /target:winexe /win32icon:assets\app.ico /out:VolumeGuard.exe ^
  /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Core.dll src\*.cs
```

На Linux/macOS собирается через Mono (`mcs` с теми же ссылками). `VolumeGuard.exe --demo` открывает интерфейс
с ненастоящими данными — удобно для проверки вёрстки без звука.

**Новая версия.** GitHub Actions собирает exe при каждом пуше в `main`. Чтобы выпустить релиз, поменяйте номер
в `VERSION` и в `src/AssemblyInfo.cs` и добавьте `[release]` в сообщение коммита — архив появится на странице Releases сам.
Как подключена подпись релизов — в [`.signpath/README.md`](.signpath/README.md).

| Файл | Что внутри |
|---|---|
| `src/CoreAudio.cs` | COM-интерфейсы Windows Core Audio |
| `src/Engine.cs` | Настройки, лимитер, работа с аудиосессиями |
| `src/Controls.cs` | Ползунки, переключатели, карточки приложений |
| `src/MainForm.cs` | Главное окно, трей, автозапуск |
| `src/AssemblyInfo.cs` | Название и версия программы |

## Настройки

Хранятся в `%APPDATA%\VolumeGuard\settings.txt` — обычный текстовый файл.
Если программа упала, подробности будут в `%APPDATA%\VolumeGuard\crash.log`.

## Конфиденциальность и изменения в системе

VolumeGuard не подключается к интернету и никуда не передаёт данные. Программа меняет только громкость
приложений и устройства, а в реестре — одну запись автозапуска (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`,
значение `VolumeGuard`), и только если вы включили «Запускать вместе с Windows». Чтобы удалить программу,
выключите автозапуск, закройте её и удалите папку с exe и `%APPDATA%\VolumeGuard`.

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

- Committers and reviewers: [@qqkillua](https://github.com/qqkillua)
- Approvers: [@qqkillua](https://github.com/qqkillua)

Signed releases are built from this repository by GitHub Actions; every signing request is approved manually.

**Privacy policy:** This program will not transfer any information to other networked systems unless specifically
requested by the user or the person installing or operating it.

## Лицензия

[MIT](LICENSE) © 2026 Nikita Bainov ([@qqkillua](https://github.com/qqkillua))

---

<details>
<summary><b>English</b></summary>

**VolumeGuard** is a per-app volume limiter for Windows 10/11. Every app that plays sound gets its own volume
slider and an optional ceiling; when an app gets louder than its ceiling, VolumeGuard smoothly turns that app
down and brings it back afterwards. There is also a device-wide limiter, a tray icon, autostart and crash-safe
volume restore. Download `VolumeGuard.zip` from [Releases](https://github.com/qqkillua/VolumeGuard/releases/latest),
unblock it in file Properties before extracting, and run `VolumeGuard.exe` (requires .NET Framework 4.x, built into
Windows 10/11). Build from source with `build.cmd`. MIT licensed.

</details>
