# Acceptance Checklist

Позначка `[x]` ставиться лише для пунктів, перевірених фактичним запуском. У дужках вказано, чим саме
перевірено. Дата перевірки — 22–23.11.2026, середовище: Windows 11 + Docker Desktop 28.4 (WSL2, 12 vCPU, 7.7 ГБ RAM).

- [x] Backend написаний на C++20. (`CMAKE_CXX_STANDARD 20`, GCC 13, каталоги `backend/`, `cli/`)
- [x] Проєкт компілюється. (Docker-цілі `runtime`, `test`, `tsan` збираються без помилок і попереджень `-Wall -Wextra -Wpedantic`)
- [x] `docker compose up` запускає систему. (запуск із порожніми volumes: dataset-init → ollama → ollama-init → search-server, healthcheck `healthy`)
- [x] Ollama є окремим сервісом. (сервіс `ollama`, образ `ollama/ollama:0.12.6`)
- [x] Ollama автоматично завантажує модель. (сервіс `ollama-init`: `ollama pull qwen2.5:3b` + прогрів, лог `model qwen2.5:3b is ready`)
- [x] Датасет автоматично створюється або завантажується. (сервіс `dataset-init`; повторний запуск пропускає генерацію)
- [x] Датасет відповідає вимогам щодо обсягу та кількості документів. (20 000 файлів, 2 145 814 475 байт ≈ 2.15 ГБ / 2.00 ГіБ)
- [x] GET /api/health повертає HTTP 200. (інтеграційні тести, E2E, curl)
- [x] POST /api/index працює. (інтеграційні тести, E2E-валідація, benchmark 15+10 побудов)
- [x] GET /api/stats працює. (інтеграційні тести, E2E)
- [x] POST /api/search працює. (інтеграційні тести, E2E, benchmark: 17 200 запитів без помилок)
- [x] Реалізовано власний Thread Pool. (`backend/include/threading/ThreadPool.hpp`, 15 unit-тестів)
- [x] Реалізовано власний Inverted Index. (`backend/include/index/InvertedIndex.hpp`, 13 + 8 unit-тестів)
- [x] Забезпечено потокобезпечний доступ. (64 шарди × `std::shared_mutex`, `ConcurrencyTest`)
- [x] Перевірено відсутність очевидних race conditions. (ціль `tsan`: 97/97 тестів під ThreadSanitizer, жодного звіту)
- [x] Query Expansion працює через Ollama. (живий стек: `car` → automobile, vehicle, sedan; E2E `expansion_status: ok`)
- [x] Реалізовано fallback при недоступності Ollama. (зупинка контейнера `ollama` під час роботи → `fallback`, лог WARN; mock-тести down/garbage/slow)
- [x] WebSocket /ws/search працює. (інтеграційні тести, E2E, Web UI)
- [x] WebSocket передає три етапи. (тест `WebSocketStreamsThreeStagesInOrder`, E2E: `query_expansion, keyword_search, done`)
- [x] CLI працює. (`docker compose exec search-server search "..." --limit N`, перевірено також fallback і недоступний сервер)
- [x] Web UI працює. (headless Chrome: desktop, mobile, fallback — `docs/report/images/`)
- [x] Unit-тести проходять. (ціль `tests`: 97 unit/concurrency тестів)
- [x] Integration-тести проходять. (28 тестів `ApiIntegrationTest`; ctest 98/98)
- [x] Concurrency-тести проходять. (`ConcurrencyTest` 7 тестів + конкурентні інтеграційні тести; також під TSan)
- [x] Benchmark виконується. (`docker compose --profile tools run --rm benchmark`, `scripts/benchmark_cold_cache.py`)
- [x] Створено два графіки продуктивності. (`docs/benchmarks/index_build_time.png`, `throughput.png` + 3 додаткові)
- [x] README відповідає реалізації. (поля та коди помилок звірено з `backend/src/api/*.cpp` та інтеграційними тестами)
- [x] UML відповідають реалізації. (`docs/uml/`, назви класів і методів з коду)
- [x] API повністю задокументований. (README §9–10, додаток А пояснювальної записки)
- [x] Пояснювальна записка створена. (`docs/report/Poiasniuvalna_zapyska.docx` + PDF, 35 сторінок, числа з CSV)
- [x] Використані промпти задокументовані. (`docs/prompts/PROMPTS_LOG.md`, `docs/prompts/master_prompt.md`)
- [x] Git має осмислену історію комітів. (Conventional Commits, атомарні коміти: `git log --oneline`)
- [ ] Публічний GitHub-репозиторій підготовлений або створений. — **Не виконано автоматично:** GitHub CLI (`gh`) у середовищі
  не встановлено, авторизації немає. Локальний репозиторій з історією комітів готовий, команди публікації наведено нижче.

## Публікація на GitHub (виконати вручну)

```bash
# варіант 1: GitHub CLI
winget install --id GitHub.cli        # або https://cli.github.com
gh auth login                         # інтерактивна авторизація в браузері
gh repo create course_work_parallel_computing --public --source . --remote origin --push

# варіант 2: без gh — створити порожній публічний репозиторій на github.com, потім:
git remote add origin https://github.com/<user>/course_work_parallel_computing.git
git push -u origin main
```

Перед публікацією: у репозиторії немає `.env`, датасету (`data/` ігнорується), моделей і бінарних файлів збірки.
Посилання на репозиторій додається на платформі здачі як **вкладення типу «Посилання»**, а не текстом у коментарі.
