# UML-діаграми

Джерела діаграм — файли Mermaid (`*.mmd`), PNG — їх відрендерені версії
(mermaid 10.9.1, headless Chrome). Діаграми відповідають коду в `backend/`.

| Діаграма | Джерело | Зображення |
|---|---|---|
| Use Case | [use_case.mmd](use_case.mmd) | [use_case.png](use_case.png) |
| Class Diagram | [class_diagram.mmd](class_diagram.mmd) | [class_diagram.png](class_diagram.png) |
| Sequence: побудова індексу (`POST /api/index`, автоіндексація) | [seq_index_build.mmd](seq_index_build.mmd) | [seq_index_build.png](seq_index_build.png) |
| Sequence: REST-пошук з Query Expansion, LLM-bulkhead і fallback (`POST /api/search`) | [seq_rest_search.mmd](seq_rest_search.mmd) | [seq_rest_search.png](seq_rest_search.png) |
| Sequence: WebSocket-пошук (`/ws/search`, Web UI) | [seq_websocket.mmd](seq_websocket.mmd) | [seq_websocket.png](seq_websocket.png) |
| Sequence: запуск системи, health/stats | [seq_health_stats.mmd](seq_health_stats.mmd) | [seq_health_stats.png](seq_health_stats.png) |
| Sequence: CLI-клієнт (`./search`) | [seq_cli.mmd](seq_cli.mmd) | [seq_cli.png](seq_cli.png) |
| Sequence: запуск benchmark і побудова графіків | [seq_benchmark.mmd](seq_benchmark.mmd) | [seq_benchmark.png](seq_benchmark.png) |
| Deployment (Docker Compose) | [deployment.mmd](deployment.mmd) | [deployment.png](deployment.png) |

Кожен прецедент діаграми Use Case має свою sequence-діаграму: пошук (REST, WebSocket, CLI), розширення
запиту та fallback (REST-діаграма), перегляд стану і статистики, побудова індексу, benchmark.

Mermaid не має окремого типу Use Case Diagram, тому вона побудована як flowchart:
актори — заокруглені вузли, прецеденти — кола, зв'язки «include»/«extend» — пунктир.
