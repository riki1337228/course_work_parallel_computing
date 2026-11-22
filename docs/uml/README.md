# UML-діаграми

Джерела діаграм — файли Mermaid (`*.mmd`), PNG — їх відрендерені версії
(mermaid 10.9.1, headless Chrome). Діаграми відповідають коду в `backend/`.

| Діаграма | Джерело | Зображення |
|---|---|---|
| Use Case | [use_case.mmd](use_case.mmd) | [use_case.png](use_case.png) |
| Class Diagram | [class_diagram.mmd](class_diagram.mmd) | [class_diagram.png](class_diagram.png) |
| Sequence: побудова індексу (`POST /api/index`, автоіндексація) | [seq_index_build.mmd](seq_index_build.mmd) | [seq_index_build.png](seq_index_build.png) |
| Sequence: REST-пошук з Query Expansion і fallback (`POST /api/search`, CLI) | [seq_rest_search.mmd](seq_rest_search.mmd) | [seq_rest_search.png](seq_rest_search.png) |
| Sequence: WebSocket-пошук (`/ws/search`, Web UI) | [seq_websocket.mmd](seq_websocket.mmd) | [seq_websocket.png](seq_websocket.png) |
| Sequence: запуск системи, health/stats | [seq_health_stats.mmd](seq_health_stats.mmd) | [seq_health_stats.png](seq_health_stats.png) |
| Deployment (Docker Compose) | [deployment.mmd](deployment.mmd) | [deployment.png](deployment.png) |

Mermaid не має окремого типу Use Case Diagram, тому вона побудована як flowchart:
актори — заокруглені вузли, прецеденти — кола, зв'язки «include»/«extend» — пунктир.
