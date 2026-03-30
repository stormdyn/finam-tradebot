# Управление секретами

## Требуемые секреты

| Переменная | Описание | Где получить |
|---|---|---|
| `FINAM_SECRET_TOKEN` | Постоянный токен Finam Trade API v2 | [tradeapi.finam.ru](https://tradeapi.finam.ru) → Апи-ключи |

## Правила

- Никогда не запекайте токен в код, `.env` файлы (даже локальные), Docker Compose, логи
- Никогда не передавайте токен через `--build-arg`, `ENV` в Dockerfile, в образ Docker
- `.env` файл запрещён в `.gitignore` (верифицируйте!)

## Локальный запуск

```bash
# Вариант 1: переменная среды (shell)
export FINAM_SECRET_TOKEN=ваш_токен
./build/finam_tradebot --dry-run

# Вариант 2: через Docker --env (НЕ --build-arg!)
docker run --rm \
  --env FINAM_SECRET_TOKEN="${FINAM_SECRET_TOKEN}" \
  finam-tradebot:latest --dry-run

# Вариант 3: connectivity check
export FINAM_SECRET_TOKEN=ваш_токен
./build/connectivity_check
# OK → запускаем бота
```

## Docker (prod)

```bash
# Создаём Docker secret (Swarm/Kubernetes)
docker secret create finam_token ./token.txt

# Или через env-file (не добавляйте в образ!)
# env-file должен быть в .gitignore и никогда не коммититься
docker run --rm --env-file .env.prod finam-tradebot:latest
```

## GitHub Actions

Добавить `FINAM_SECRET_TOKEN` как **Repository Secret** в:
`Settings → Secrets and variables → Actions → New repository secret`

```yaml
# Пример использования в workflow (только для integration tests):
- name: Connectivity check
  env:
    FINAM_SECRET_TOKEN: ${{ secrets.FINAM_SECRET_TOKEN }}
  run: ./build/connectivity_check
```

## Ротация токена

1. Зайти на tradeapi.finam.ru → отозвать старый
2. Сгенерировать новый
3. Обновить Repository Secret в GitHub
4. Перезапустить бота: `docker stop` → `docker run` с новым токеном
5. Убедиться что `connectivity_check` возвращает 0
