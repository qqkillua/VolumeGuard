# Подпись релизов через SignPath

Сборка уже готова к подписи: шаг «Sign with SignPath» в `.github/workflows/build.yml`
включится сам, как только в репозитории появятся настройки ниже. До этого релизы выходят без подписи.

## 1. Подготовка

- Включите двухфакторную аутентификацию на GitHub (Settings → Password and authentication). SignPath требует её от всех участников проекта.

## 2. Заявка

Подайте заявку на бесплатную подпись для open source на [signpath.org](https://signpath.org/).
Ссылка на репозиторий: `https://github.com/qqkillua/VolumeGuard`. Раздел «Code signing policy» уже есть в README.

## 3. Настройка после одобрения

В SignPath:

1. Установите SignPath GitHub App на репозиторий VolumeGuard.
2. В проекте добавьте **Trusted Build System → GitHub.com**.
3. **Artifact configuration** — вставьте содержимое [`artifact-configuration.xml`](artifact-configuration.xml).
4. Создайте API-токен для пользователя с правом отправки запросов на подпись (Submitter).

В GitHub (Settings → Secrets and variables → Actions):

| Тип | Имя | Значение |
|---|---|---|
| Secret | `SIGNPATH_API_TOKEN` | API-токен из SignPath |
| Variable | `SIGNPATH_ORGANIZATION_ID` | ID организации в SignPath |
| Variable | `SIGNPATH_PROJECT_SLUG` | slug проекта, например `VolumeGuard` |
| Variable | `SIGNPATH_POLICY_SLUG` | slug политики подписи, например `release-signing` |

## 4. Выпуск подписанной версии

1. Поменяйте номер в `VERSION` и в `src/AssemblyInfo.cs` (иначе сборка остановится с ошибкой).
2. Закоммитьте с `[release]` в сообщении.
3. Подтвердите запрос на подпись в SignPath — сборка ждёт до часа.
4. Подписанный `VolumeGuard.zip` появится в Releases.
