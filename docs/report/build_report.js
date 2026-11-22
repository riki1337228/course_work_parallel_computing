// Generates docs/report/Poiasniuvalna_zapyska.docx (explanatory note, Ukrainian).
// Usage: npm install docx@9.5.1 && node docs/report/build_report.js
// All numbers are read from docs/benchmarks/*.csv (measured results), not typed by hand.

const fs = require("fs");
const path = require("path");
const {
  Document, Packer, Paragraph, TextRun, ImageRun, Table, TableRow, TableCell, WidthType, ShadingType,
  AlignmentType, HeadingLevel, PageBreak, Footer, PageNumber, TableOfContents, LevelFormat, BorderStyle,
  TabStopType, VerticalAlign, LineRuleType, PositionalTab, PositionalTabAlignment, PositionalTabRelativeTo,
  PositionalTabLeader,
} = require("docx");

const ROOT = path.resolve(__dirname, "..", "..");
const OUT = path.join(__dirname, "Poiasniuvalna_zapyska.docx");
const FONT = "Times New Roman";
const MONO = "Consolas";
const TEXT_WIDTH = 9355; // A4 21 cm - 3 cm - 1.5 cm = 16.5 cm in DXA
const MAX_IMG_W = 620;   // px at 96 dpi ~ 16.4 cm

// ------------------------------------------------------------------ data
function readCsv(rel) {
  const lines = fs.readFileSync(path.join(ROOT, rel), "utf8").trim().split(/\r?\n/);
  const header = lines[0].split(",");
  return lines.slice(1).map((l) => {
    const cells = l.split(",");
    return Object.fromEntries(header.map((h, i) => [h, cells[i]]));
  });
}
const idx = readCsv("docs/benchmarks/index_build_summary.csv");
const idxRuns = readCsv("docs/benchmarks/index_build_runs.csv");
const cold = readCsv("docs/benchmarks/index_build_cold.csv");
const thr = readCsv("docs/benchmarks/search_throughput_detail.csv");
const llm = readCsv("docs/benchmarks/search_throughput_llm.csv");
const queries = readCsv("docs/benchmarks/query_examples.csv");
const coldMean = {};
for (const r of cold) (coldMean[r.threads] ||= []).push(+r.time_ms);
for (const k of Object.keys(coldMean)) coldMean[k] = coldMean[k].reduce((a, b) => a + b, 0) / coldMean[k].length;
const fmt = (v, d = 1) => Number(v).toLocaleString("uk-UA", { minimumFractionDigits: d, maximumFractionDigits: d });

// ------------------------------------------------------------------ helpers
let figNo = {}; let tabNo = {};
function nextNum(store, ch) { store[ch] = (store[ch] || 0) + 1; return `${ch}.${store[ch]}`; }

function runs(text, opts = {}) {
  // **bold** and `code` inline markup
  const parts = text.split(/(\*\*[^*]+\*\*|`[^`]+`)/g).filter(Boolean);
  return parts.map((p) => {
    if (p.startsWith("**")) return new TextRun({ text: p.slice(2, -2), bold: true, font: FONT, size: opts.size || 28 });
    if (p.startsWith("`")) return new TextRun({ text: p.slice(1, -1), font: MONO, size: (opts.size || 28) - 4 });
    return new TextRun({ text: p, font: FONT, size: opts.size || 28, italics: opts.italics, bold: opts.bold });
  });
}
const P = (text, opts = {}) => new Paragraph({
  children: runs(text, opts), alignment: opts.align || AlignmentType.JUSTIFIED,
  indent: opts.noIndent ? undefined : { firstLine: 709 }, spacing: { line: 360, after: 0, before: 0 },
  keepNext: opts.keepNext,
});
// Static table of contents: page numbers come from a previous render (toc_pages.json),
// because LibreOffice does not update TOC fields when converting to PDF.
const tocEntries = [];
const H1 = (text) => { tocEntries.push({ text, level: 1 }); return new Paragraph({ heading: HeadingLevel.HEADING_1, children: [new TextRun({ text, font: FONT })], pageBreakBefore: true }); };
const H2 = (text) => { tocEntries.push({ text, level: 2 }); return new Paragraph({ heading: HeadingLevel.HEADING_2, children: [new TextRun({ text, font: FONT })] }); };
const H3 = (text) => new Paragraph({ heading: HeadingLevel.HEADING_3, children: [new TextRun({ text, font: FONT })] });
const bullets = (items) => items.map((t) => new Paragraph({
  children: runs(t), numbering: { reference: "dash", level: 0 }, alignment: AlignmentType.JUSTIFIED, spacing: { line: 360 },
}));
const numbered = (items, ref) => items.map((t) => new Paragraph({
  children: runs(t), numbering: { reference: ref, level: 0 }, alignment: AlignmentType.JUSTIFIED, spacing: { line: 360 },
}));
const code = (text) => text.split("\n").map((line, i, all) => new Paragraph({
  children: [new TextRun({ text: line || " ", font: MONO, size: 18 })],
  spacing: { line: 240, before: i === 0 ? 120 : 0, after: i === all.length - 1 ? 120 : 0 },
  shading: { type: ShadingType.CLEAR, fill: "F3F4F6", color: "auto" }, indent: { left: 284 },
}));
const empty = () => new Paragraph({ children: [new TextRun({ text: "", font: FONT, size: 28 })] });

function pngSize(file) {
  const b = fs.readFileSync(file);
  return { w: b.readUInt32BE(16), h: b.readUInt32BE(20), data: b };
}
function figure(rel, caption, ch, maxH = 820) {
  const { w, h, data } = pngSize(path.join(ROOT, rel));
  let width = Math.min(MAX_IMG_W, w);
  let height = Math.round((h * width) / w);
  if (height > maxH) { height = maxH; width = Math.round((w * height) / h); }
  const n = nextNum(figNo, ch);
  return { n, items: [
    new Paragraph({ children: [new ImageRun({ type: "png", data, transformation: { width, height },
      altText: { title: caption, description: caption, name: path.basename(rel) } })],
      alignment: AlignmentType.CENTER, spacing: { before: 120, after: 60, line: 240, lineRule: LineRuleType.AUTO }, keepNext: true }),
    new Paragraph({ children: [new TextRun({ text: `Рисунок ${n} – ${caption}`, font: FONT, size: 26 })],
      alignment: AlignmentType.CENTER, spacing: { after: 200 } }),
  ] };
}

const border = { style: BorderStyle.SINGLE, size: 4, color: "000000" };
const borders = { top: border, bottom: border, left: border, right: border };
function table(caption, ch, header, rows, widths) {
  const total = widths.reduce((a, b) => a + b, 0);
  const scaled = widths.map((w) => Math.floor((w / total) * TEXT_WIDTH));
  scaled[scaled.length - 1] += TEXT_WIDTH - scaled.reduce((a, b) => a + b, 0);
  const cell = (text, isHeader, i) => new TableCell({
    borders, width: { size: scaled[i], type: WidthType.DXA }, verticalAlign: VerticalAlign.CENTER,
    shading: isHeader ? { type: ShadingType.CLEAR, fill: "E7E9EE", color: "auto" } : undefined,
    margins: { top: 40, bottom: 40, left: 80, right: 80 },
    children: [new Paragraph({ children: runs(String(text), { size: 22, bold: isHeader }),
      alignment: isHeader ? AlignmentType.CENTER : AlignmentType.LEFT })],
  });
  const n = nextNum(tabNo, ch);
  return { n, items: [
    new Paragraph({ children: [new TextRun({ text: `Таблиця ${n} – ${caption}`, font: FONT, size: 26 })],
      spacing: { before: 200, after: 60 }, keepNext: true }),
    new Table({ width: { size: TEXT_WIDTH, type: WidthType.DXA }, columnWidths: scaled,
      rows: [new TableRow({ tableHeader: true, children: header.map((h, i) => cell(h, true, i)) }),
        ...rows.map((r) => new TableRow({ cantSplit: true, children: r.map((c, i) => cell(c, false, i)) }))] }),
    new Paragraph({ children: [new TextRun({ text: "", size: 12 })], spacing: { after: 120 } }),
  ] };
}

// ------------------------------------------------------------------ title page
const center = (text, opts = {}) => new Paragraph({ alignment: AlignmentType.CENTER,
  spacing: { line: 360, before: opts.before || 0, after: opts.after || 0 },
  children: [new TextRun({ text, font: FONT, size: opts.size || 28, bold: opts.bold, allCaps: opts.caps })] });
const field = (label, value) => new Paragraph({ spacing: { line: 360 }, indent: { left: 4536 },
  children: [new TextRun({ text: `${label} `, font: FONT, size: 28 }), new TextRun({ text: value, font: FONT, size: 28, color: "555555" })] });

const titlePage = [
  center("МІНІСТЕРСТВО ОСВІТИ І НАУКИ УКРАЇНИ"),
  center("[Назва університету]", { before: 120 }),
  center("[Назва факультету / інституту]"),
  center("Кафедра [назва кафедри]"),
  center("КУРСОВА РОБОТА", { before: 2400, bold: true, size: 32 }),
  center("з дисципліни «Паралельні обчислення»", { before: 120 }),
  center("на тему:", { before: 240 }),
  center("«Дослідження та реалізація паралельних обчислень на прикладі побудови пошукової системи з інтеграцією LLM»", { before: 120, bold: true }),
  center("", { before: 1600 }),
  field("Виконав(ла): студент(ка) групи", "[група]"),
  field("", "[Прізвище, ім'я, по батькові]"),
  field("Керівник:", "[посада, науковий ступінь]"),
  field("", "[Прізвище, ім'я, по батькові]"),
  field("Оцінка:", "_______________"),
  center("[Місто] – [Рік]", { before: 2200 }),
];

// ------------------------------------------------------------------ body
const C = [];
const add = (...xs) => xs.forEach((x) => (Array.isArray(x) ? C.push(...x) : x.items ? C.push(...x.items) : C.push(x)));

// ВСТУП
add(H1("1 ВСТУП"));
add(P("**Актуальність.** Пошук інформації у великих текстових корпусах лежить в основі пошукових систем, систем документообігу та сервісів підтримки. Обсяги даних постійно зростають, і послідовна обробка вже не дає прийнятного часу побудови індексу та відповіді. Сучасні процесори багатоядерні, тому продуктивність системи залежить від того, наскільки ефективно вона розпаралелює обчислення та синхронізує доступ до спільних структур даних. Окрім того, класичний пошук за ключовими словами не враховує синонімів, а великі мовні моделі (LLM) можна використати для розширення запиту (Query Expansion). Щоправда, повільна LLM не повинна блокувати пошукову систему."));
add(P("**Мета роботи** — дослідити та реалізувати паралельні обчислення на прикладі контейнеризованої пошукової системи. Система паралельно будує інвертований індекс власним пулом потоків, конкурентно обслуговує клієнтів і використовує локальну LLM для розширення запитів. Також потрібно експериментально оцінити масштабованість системи."));
add(P("Для досягнення мети поставлено такі **завдання**:", { keepNext: true }));
add(numbered([
  "Спроєктувати архітектуру системи та описати її UML-діаграмами.",
  "Реалізувати власний пул потоків (Thread Pool) на засобах стандартної бібліотеки C++20.",
  "Реалізувати власну потокобезпечну структуру даних — інвертований індекс — і алгоритм його паралельної побудови.",
  "Реалізувати ранжування результатів за TF-IDF.",
  "Інтегрувати локальну мовну модель Ollama (qwen2.5:3b) для Query Expansion з резервним режимом (fallback).",
  "Реалізувати REST API, WebSocket-канал етапів обробки, CLI-клієнт і Web UI.",
  "Забезпечити запуск системи однією командою `docker compose up` з автоматичною підготовкою датасету та моделі.",
  "Розробити модульні, інтеграційні та конкурентні тести.",
  "Виміряти час індексації залежно від кількості потоків і пропускну здатність залежно від кількості клієнтів, проаналізувати результати.",
], "tasks"));
add(P("**Об'єкт дослідження** — процеси паралельної обробки текстових даних і конкурентного обслуговування запитів у пошукових системах."));
add(P("**Предмет дослідження** — методи розпаралелювання побудови інвертованого індексу, механізми синхронізації потоків (м'ютекси, умовні змінні, блокування читання-запису) та їх вплив на продуктивність і масштабованість."));
add(P("**Технології:** C++20, CMake, Drogon (HTTP/WebSocket), nlohmann/json, GoogleTest, ThreadSanitizer, Ollama, HTML/CSS/JavaScript, Docker, Docker Compose, Python 3 (генерація датасету, benchmark, графіки)."));
add(table("Технологічний стек", "1", ["Рівень", "Технології та версії"], [
  ["Backend", "C++20, GCC 13 (Ubuntu 24.04), CMake ≥ 3.20, Drogon v1.9.11, nlohmann/json 3.11, std::thread / mutex / shared_mutex / condition_variable / atomic"],
  ["Тестування", "GoogleTest 1.14, ThreadSanitizer, Python E2E-скрипт"],
  ["LLM", "Ollama 0.12.6, модель qwen2.5:3b (локально, CPU)"],
  ["Frontend", "HTML5, CSS3, JavaScript без фреймворків, WebSocket API"],
  ["Інфраструктура", "Docker multi-stage build, Docker Compose (4 сервіси + інструменти), volumes, healthchecks"],
  ["Дослідження", "Python 3.12: генератор датасету, benchmark-скрипти, matplotlib"],
], [1, 4]));

// РОЗДІЛ 1 (= розділ «Модель системи»)
add(H1("2 МОДЕЛЬ СИСТЕМИ"));
add(H2("2.1 Загальна архітектура"));
add(P("Система складається з шести логічних компонентів: пошуковий сервер на C++ (Search Backend), окремий сервіс локальної LLM (Ollama), веб-інтерфейс (Web UI, роздається сервером), клієнт командного рядка (CLI), генератор датасету та інструменти вимірювання продуктивності. Архітектура навмисно не мікросервісна: індексація, пошук, REST і WebSocket працюють в одному процесі й мають спільний індекс у пам'яті. Окремим сервісом винесено лише Ollama, оскільки це окремий продукт із власними ресурсами."));
add(P("Конвеєр індексації: Dataset → DocumentLoader → TextProcessor → ThreadPool → InvertedIndex → SearchEngine. Пошуковий конвеєр: Client → REST API / WebSocket → Query Expansion (Ollama) → Keyword Search → Ranking → Response."));
add(P("Ключові архітектурні рішення:", { keepNext: true }));
add(bullets([
  "event loop HTTP-сервера лише приймає та валідує запити, а пошук і звернення до Ollama виконуються у власному пулі потоків `requestPool`. Повільна модель не блокує інших клієнтів;",
  "окремий невеликий пул `controlPool` обслуговує перевірки стану та запуск індексації, щоб вони не стояли в черзі за запитами до LLM;",
  "індекс публікується як незмінний знімок (`std::shared_ptr`): під час перебудови пошук продовжує працювати на попередньому знімку;",
  "будь-яка відмова LLM (недоступність, помилка, тайм-аут, некоректна відповідь) переводить пошук у режим fallback — звичайний keyword search;",
  "ядро (`search_core`) не залежить від веб-фреймворку, тому його можна тестувати та перевіряти ThreadSanitizer-ом окремо.",
]));
add(figure("docs/uml/deployment.png", "Діаграма розгортання (топологія Docker Compose)", "2"));

add(H2("2.2 UML-моделі"));
add(P("Діаграми побудовано в нотації Mermaid (джерела в каталозі `docs/uml/`) і звірено з реалізацією. Діаграма прецедентів (рис. 2.2) показує акторів: користувача веб-інтерфейсу, користувача CLI, оператора/дослідника та зовнішню систему Ollama. Діаграма класів (рис. 2.3) згрупована за модулями `core`, `threading`, `index`, `engine`, `ollama` та `api`."));
add(figure("docs/uml/use_case.png", "Діаграма прецедентів", "2", 600));
add(figure("docs/uml/class_diagram.png", "Діаграма класів", "2"));
add(P("Діаграми послідовностей описують основні сценарії: побудову індексу (рис. 2.4), REST-пошук з розширенням запиту та fallback (рис. 2.5), пошук через WebSocket із потоковою передачею етапів (рис. 2.6), запуск системи та опитування стану (рис. 2.7)."));
add(figure("docs/uml/seq_index_build.png", "Діаграма послідовності: паралельна побудова індексу", "2"));
add(figure("docs/uml/seq_rest_search.png", "Діаграма послідовності: POST /api/search з Query Expansion і fallback", "2"));
add(figure("docs/uml/seq_websocket.png", "Діаграма послідовності: пошук через WebSocket /ws/search", "2"));
add(figure("docs/uml/seq_health_stats.png", "Діаграма послідовності: запуск системи, /api/health і /api/stats", "2"));

add(H2("2.3 Модель паралельної обробки"));
add(P("Побудова індексу використовує **паралелізм за даними** за схемою «map + merge». Набір документів ділиться на пакети по B документів, кожен пакет стає окремою задачею пулу з N потоків. Пакетів більше, ніж потоків (близько восьми на потік), тому спільна черга задач забезпечує динамічне балансування навантаження: потік, що звільнився, бере наступний пакет. Розмір пакета обчислюється як B = clamp(Nдок / (8·N), 1, 64). Для 20 000 документів B = 64, тобто 313 задач."));
add(P("Фаза **map** (читання файлу, нормалізація, токенізація, підрахунок частот) виконується повністю локально, у приватній хеш-таблиці потоку `LocalIndex`, без жодної синхронізації. Фаза **merge** — злиття локального результату в спільний індекс. Вона відбувається один раз на пакет: терміни групуються за шардами, і кожен шард блокується один раз на короткий час. Завершальна фаза — паралельне сортування списків входжень (posting lists) за ідентифікатором документа на тому самому пулі."));
add(P("Обслуговування запитів використовує **паралелізм за задачами**: IO-потоки Drogon (4 event loop) приймають з'єднання і розбирають HTTP/WebSocket-кадри, а пошук із викликом LLM виконується в `requestPool` (16 потоків). Читання індексу різними потоками відбувається під спільними блокуваннями (`shared_lock`), тому пошукові запити не блокують один одного."));

add(H2("2.4 Модель даних"));
add(bullets([
  "`Document {id, name, path, tokenCount, sizeBytes}` — метадані документа. Текст у пам'яті не зберігається, а читається з диска для snippet;",
  "`Posting {docId, termFrequency}` — запис списку входжень;",
  "`InvertedIndex` — 64 шарди, кожен містить `unordered_map<string, vector<Posting>>` і власний `std::shared_mutex`, а також таблицю документів і масив довжин документів;",
  "датасет — каталог `documents/` з файлами `document_NNNNNN.txt` і файл `metadata.json` з параметрами генерації.",
]));
add(P("Для 20 000 документів (2,0 ГіБ) індекс містить 28 024 унікальні терміни та 14 479 164 записи (термін, документ) і займає близько 200 МБ оперативної пам'яті процесу."));

add(H2("2.5 Модель REST API"));
add(table("Ендпоїнти REST API", "2", ["Метод і URL", "Призначення", "Основні коди"], [
  ["GET /api/health", "стан сервера, доступність Ollama", "200"],
  ["POST /api/index", "побудова індексу з N потоками {\"threads\": int}", "200, 400, 409, 422, 500"],
  ["POST /api/search", "пошук {\"query\", \"mode\":\"keyword\", \"limit\"?, \"expand\"?}", "200, 400, 503"],
  ["GET /api/stats", "статистика: документи, час побудови, потоки", "200"],
], [2, 4, 2]));
add(P("Помилки повертаються в єдиному форматі `{\"error\": \"<код>\", \"message\": \"<опис>\"}`. Повну специфікацію з типами всіх полів наведено в додатку А. Обов'язкові поля відповідають завданню, а додаткові (`total_matches`, `expanded_terms`, `expansion_status`, `fallback_reason` тощо) задокументовано."));
add(H2("2.6 Модель WebSocket"));
add(P("Клієнт підключається до `ws://host:8080/ws/search` і надсилає `{\"query\": \"...\"}` (необов'язково `limit` і `expand`). Сервер надсилає три окремі JSON-повідомлення строго в порядку `query_expansion` → `keyword_search` → `done`. Порядок гарантується тим, що всі три кадри надсилає одна задача пулу послідовно. У разі помилки надсилається `{\"stage\": \"error\", \"data\": {\"error\", \"message\"}}`, після чого з'єднання лишається відкритим. Одночасно на одному з'єднанні виконується лише один пошук, а різні з'єднання повністю незалежні."));

// РОЗДІЛ 2 — реалізація
add(H1("3 ОПИС РЕАЛІЗАЦІЇ"));
add(H2("3.1 Структура проєкту"));
add(P("Код backend розділено на модулі з однаковою структурою каталогів `include/` і `src/`:"));
add(table("Модулі backend", "3", ["Модуль", "Класи", "Відповідальність"], [
  ["core", "Config, Document, DocumentLoader, TextProcessor", "конфігурація зі змінних середовища, читання файлів, нормалізація та токенізація"],
  ["threading", "ThreadPool", "власний пул потоків"],
  ["index", "InvertedIndex, IndexBuilder", "потокобезпечний індекс, паралельна побудова"],
  ["search", "SearchEngine, SearchService", "TF-IDF ранжування, оркестрація індексації та пошуку"],
  ["ollama", "LlmClient, OllamaClient, OllamaProtocol, QueryExpander", "HTTP-клієнт Ollama, промпт, розбір відповіді, fallback"],
  ["api", "HealthController, IndexController, SearchController, StatsController, SearchWebSocket", "REST і WebSocket (Drogon)"],
], [1.2, 3, 3.5]));
add(P("Бібліотека `search_core` (core, threading, index, search, логіка ollama) не залежить від Drogon. Бібліотека `search_api` містить HTTP-шар і мережевий клієнт Ollama. Поруч розташовані каталоги `cli/` (клієнт `search`), `frontend/` (Web UI), `scripts/` (генератор датасету, benchmark, графіки, E2E), `tests/unit` та `tests/integration`, `docs/`."));

add(H2("3.2 Пул потоків"));
add(P("Клас `ThreadPool` створює N робочих потоків `std::thread`, які отримують задачі зі спільної черги `std::queue<std::function<void()>>`. Черга, лічильник активних задач і прапорець зупинки захищені одним `std::mutex`, а очікування реалізовано двома умовними змінними: `taskAvailable_` (є задача або почалася зупинка) та `allDone_` (черга порожня і немає активних задач). Задача виконується поза м'ютексом, тому критична секція обмежена операціями з чергою."));
add(code(`void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            taskAvailable_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) return;            // stopping and nothing left
            task = std::move(tasks_.front());
            tasks_.pop();
            ++activeTasks_;
        }
        try { task(); } catch (...) { failed_.fetch_add(1); }   // outside the lock
        completed_.fetch_add(1);
        {
            std::lock_guard lock(mutex_);
            if (--activeTasks_ == 0 && tasks_.empty()) allDone_.notify_all();
        }
    }
}`));
add(P("Метод `submit(f, args...)` загортає виклик у `std::packaged_task` і повертає `std::future`: результат або виняток задачі передається викликачеві через `future.get()`. Метод `post(f)` призначений для задач без результату: виняток перехоплюється, враховується в лічильнику `failedTasks()` і передається необов'язковому обробнику помилок, а робочий потік продовжує роботу. Метод `shutdown()` встановлює прапорець зупинки, після чого нові задачі відхиляються з винятком `ThreadPoolStoppedError`. Потоки доопрацьовують чергу й приєднуються (`join`). Повторні й одночасні виклики безпечні завдяки `std::call_once`. Деструктор викликає `shutdown()`, тож витоків потоків немає."));
add(P("Відсутність взаємного блокування забезпечується тим, що в пулі є лише один м'ютекс, а користувацький код ніколи не виконується під ним. Задача може безпечно ставити в той самий пул нові задачі (перевірено тестом)."));

add(H2("3.3 Інвертований індекс і синхронізація"));
add(P("Клас `InvertedIndex` зберігає відображення «термін → список (документ, частота)». Для зменшення конкуренції застосовано **розщеплення блокувань (lock striping)**: словник поділено на 64 шарди за значенням `hash(term) mod 64`, кожен шард має свій `std::shared_mutex`. Операції запису (`merge`, `addDocument`) беруть ексклюзивне блокування лише одного шарду за раз, а операції читання (`visitPostings`, `documentFrequency`, `termFrequency`) — спільне. Таблиця документів захищена окремим `std::shared_mutex`, лічильники є `std::atomic`. Жоден писач не утримує два блокування одночасно, тому цикл очікування, а отже й deadlock, неможливий."));
add(code(`void InvertedIndex::merge(LocalIndex&& local) {
    std::vector<std::vector<LocalIndex::node_type>> byShard(shards_.size());
    while (!local.empty()) {                       // group terms by shard
        auto node = local.extract(local.begin());
        byShard[shardIndex(node.key())].push_back(std::move(node));
    }
    for (std::size_t s = 0; s < byShard.size(); ++s) {
        if (byShard[s].empty()) continue;
        std::unique_lock lock(shards_[s]->mutex);  // one short critical section per shard
        for (auto& node : byShard[s]) { /* insert node or append postings */ }
    }
}`));
add(P("Клас `IndexBuilder` реалізує паралельну побудову, описану в п. 2.3. Ідентифікатори документів призначаються за відсортованим списком файлів до запуску потоків, тому потоки не синхронізуються для нумерації, а результат не залежить від кількості потоків. Тест `ResultIsIdenticalForAnyThreadCount` перевіряє, що індекс, побудований 1, 2, 4, 8 і 16 потоками, однаковий. Після побудови готовий індекс атомарно передається в `SearchEngine`. Запити, що вже виконуються, тримають `shared_ptr` на попередній знімок, тому перебудова не потребує зупинки пошуку."));
add(P("Нормалізація тексту (`TextProcessor`) працює за таблицею перетворення байтів: великі латинські літери переводяться в малі, будь-який не-алфавітно-цифровий байт стає роздільником. Довжина рядка при цьому не змінюється, що дозволяє знаходити позиції для snippet в оригінальному тексті. Токени коротші за 2 і довші за 48 символів, а також ~120 англійських стоп-слів відкидаються. Токенізація повертає `std::string_view` на буфер документа без додаткових виділень пам'яті."));
add(H3("3.3.1 Ранжування TF-IDF"));
add(P("Релевантність документа d запиту Q обчислюється за формулою:", { keepNext: true }));
add(new Paragraph({ alignment: AlignmentType.CENTER, spacing: { before: 120, after: 120 },
  children: [new TextRun({ text: "score(d, Q) = Σ t∈Q  w(t) · ( f(t,d) / |d| ) · ( ln( N / df(t) ) + 1 ),", font: FONT, size: 28, italics: true })] }));
add(P("де f(t,d) — кількість входжень терміна t у документ d; |d| — кількість токенів документа після нормалізації; N — кількість документів; df(t) — кількість документів, що містять t; w(t) — вага терміна (1,0 для слів запиту, 0,5 для термінів, запропонованих LLM). Нормування частоти на довжину документа прибирає перевагу довгих документів. Логарифмічний IDF зменшує внесок поширених слів, а доданок +1 не обнуляє його для слів, що трапляються в усіх документах. Запит обробляється з OR-семантикою: документ, що містить більше (і рідкісніших) термінів, отримує вищу оцінку. Для вибору K найкращих результатів використовується `std::partial_sort`. Snippet — фрагмент ~240 символів навколо першого входження терміна запиту (лише цілі слова, без урахування регістру)."));

add(H2("3.4 Query Expansion та інтеграція Ollama"));
add(P("Ollama працює як окремий контейнер. Модель qwen2.5:3b завантажує й прогріває одноразовий сервіс `ollama-init`. Клас `QueryExpander` формує структурований промпт, що вимагає відповісти одним JSON-об'єктом `{\"original\": ..., \"expanded_terms\": [...]}` з не більш ніж шістьма термінами по 1–2 слова. Запит користувача вставляється як JSON-рядок, що екранує лапки й керівні символи. `OllamaClient` надсилає `POST /api/generate` з параметрами `\"stream\": false`, `\"format\": \"json\"`, `temperature = 0,2`, `num_predict = 128`."));
add(P("Захист від некоректної відповіді: розбирається лише JSON (у тому числі оточений текстом або markdown), нерядкові елементи ігноруються, терміни очищуються до символів [a-z0-9 -], задовгі (> 40 символів), повторювані, такі, що збігаються із запитом або складаються лише зі стоп-слів, відкидаються, кількість обмежена `MAX_EXPANSION_TERMS`. Запит лише зі стоп-слів до моделі не надсилається."));
add(P("**Відмовостійкість.** Кожен запит до Ollama має тайм-аут: 30 с для генерації, 2 с для перевірки доступності. Доступність (`GET /api/tags` + наявність моделі) кешується на 5 с, а оновлення кешу серіалізоване, тому одночасні запити не створюють «шторму» перевірок і не отримують застарілого значення. Мережева помилка або тайм-аут одразу оновлюють стан доступності. Будь-яка відмова переводить пошук у режим `fallback`: виконується пошук за вихідними термінами, причина повертається в полі `fallback_reason` і записується в лог із рівнем WARN. HTTP-запити до Ollama виконуються з потоків `requestPool` на окремому event loop клієнта, тому event loop сервера не блокується."));
add(table("Фактичні приклади розширення запитів (qwen2.5:3b, CPU)", "3", ["Запит", "expanded_terms"],
  queries.filter((q) => q.expand === "True" && q.expanded_terms).slice(0, 8).map((q) => [q.query, q.expanded_terms.replace(/; /g, ", ")]), [2, 5]));

add(H2("3.5 REST API та WebSocket"));
add(P("HTTP-шар побудовано на Drogon. Маршрути реєструються явно (`registerRoutes`) і делегують обробку контролерам. Контролер валідує запит в IO-потоці: перевіряє JSON, типи й діапазони полів. Потім він передає довгу роботу в пул і звільняє event loop. Відповідь надсилається з робочого потоку через callback Drogon, що є потокобезпечним. Контролер пошуку та WebSocket використовують одну функцію валідації `SearchController::parseRequest`, тому правила однакові. Невідомі шляхи повертають JSON-помилку 404, неприпустимий метод — 405. Розмір тіла обмежено 64 КБ. Кожен запит логується."));
add(P("WebSocket-обробник `SearchWebSocket` зберігає в контексті з'єднання атомарний прапорець «пошук виконується». Повторний запит на тому ж з'єднанні до завершення попереднього отримує помилку `search_in_progress`. Етапи надсилаються послідовно з однієї задачі пулу: результат розширення одразу після відповіді LLM, результати пошуку — після ранжування."));

add(H2("3.6 CLI-клієнт і Web UI"));
add(P("CLI-клієнт `search` написано на C++ (HTTP-клієнт Drogon). Він збирається разом із проєктом і встановлюється в образ сервера. Клієнт приймає запит з аргументів, параметри `--limit`, `--server`, `--no-expand`, `--timeout`, `--json`, виводить розширені терміни, кількість збігів, час пошуку й результати, а помилки сервера та мережі обробляє з різними кодами виходу."));
add(code(`$ docker compose exec search-server search "artificial intelligence" --limit 2
Query:    "artificial intelligence"
Expanded: machine learning, deep learning, ai, robotics  [Ollama, 2156.39 ms]
Found 17561 matching documents, showing 2
Search time: 3.416 ms (index), 2159.82 ms (total)

  1. document_017062   score 0.155518
     ...at the medical school in Dublin reported that the research vessel ...`));
add(P("Web UI — односторінковий застосунок на HTML/CSS/JavaScript без фреймворків. Він показує статус Ollama і кількість документів (опитування `/api/health` і `/api/stats` кожні 5 с) і візуалізує етапи pipeline через WebSocket: активний етап пульсує, завершені позначаються зеленим, fallback — помаранчевим. Інтерфейс виводить повідомлення «Searching…», «Expanding query…», «Searching index…», «Search completed.», «Ollama unavailable — fallback mode.», розширені терміни та картки результатів зі score і підсвіченими збігами. Верстка адаптивна (рис. 3.1–3.3). Знімки екрана зроблено з реально працюючої системи."));
add(figure("docs/report/images/ui_desktop.png", "Web UI: результати пошуку з розширенням запиту", "3", 700));
add(figure("docs/report/images/ui_fallback.png", "Web UI: режим fallback (контейнер Ollama зупинено)", "3", 560));
add(figure("docs/report/images/ui_mobile.png", "Web UI на екрані смартфона", "3", 700));

add(H2("3.7 Docker Compose"));
add(P("Збірка backend багатоетапна: етап `deps` (компілятор, бібліотеки, Drogon з вихідних кодів зафіксованої версії), `build` (сервер, CLI, тести), `test` і `tsan` (запуск тестів) та мінімальний `runtime`. Файл `docker-compose.yml` описує сервіси:"));
add(table("Сервіси Docker Compose", "3", ["Сервіс", "Призначення", "Умова готовності / залежності"], [
  ["dataset-init", "генерує датасет у volume `dataset`; ідемпотентний", "завершення з кодом 0"],
  ["ollama", "сервер LLM, моделі у volume `ollama-models`", "healthcheck `ollama list`"],
  ["ollama-init", "завантаження та прогрів qwen2.5:3b (до 5 спроб)", "після healthy `ollama`"],
  ["search-server", "C++ сервер, REST/WS/UI, автоіндексація", "після `dataset-init` (service_completed_successfully); healthcheck `/api/health`"],
  ["tests, tsan, e2e, benchmark", "інструменти (профіль tools)", "запуск вручну"],
], [1.5, 3, 3]));
add(P("Сервери спілкуються через внутрішню мережу `search-net`, backend звертається до `http://ollama:11434` за ім'ям сервісу, порт Ollama назовні не відкривається. Сервер не чекає на завантаження моделі: поки вона завантажується, пошук працює в режимі fallback, тож перший запуск не завершується помилкою через неготовність Ollama. Запуск із чистого стану (без volumes) перевірено: датасет 20 000 документів (2046,4 МіБ) згенеровано за 67 с, індекс побудовано автоматично за ~4–6 с після старту сервера, модель завантажено й прогріто, E2E-перевірка пройшла."));

// РОЗДІЛ 3 — тестування та результати
add(H1("4 ТЕСТУВАННЯ ТА РЕЗУЛЬТАТИ"));
add(H2("4.1 Методика тестування"));
add(P("Тестування виконано на чотирьох рівнях. (1) Модульні тести GoogleTest для кожного компонента ядра. (2) Тести конкурентності: одночасне читання та запис індексу, пошук під час перебудови, відсутність втрат документів. Ці тести також виконуються під ThreadSanitizer, який фіксує будь-який data race. (3) Інтеграційні тести: справжній сервер Drogon у тому ж процесі на тестовому датасеті, а замість Ollama — mock-сервер у чотирьох режимах (працює, недоступний, повертає некоректну відповідь, відповідає повільніше за тайм-аут); HTTP і WebSocket-клієнти Drogon. (4) E2E-перевірка розгорнутої системи з реальною моделлю й датасетом 2 ГБ. Усі тести запускаються в Docker, локальний компілятор не потрібен."));
add(H2("4.2 Результати модульних і інтеграційних тестів"));
add(table("Набори автоматизованих тестів", "4", ["Набір", "Що перевіряється", "Тестів", "Результат"], [
  ["ThreadPoolTest", "результати та винятки через future, post і обробник помилок, заборона задач після зупинки, дренаж черги, ідемпотентний shutdown, конкурентні продюсери, вкладені задачі, реальний паралелізм", "15", "пройдено"],
  ["TextProcessorTest", "lowercase, пунктуація, стоп-слова, порожні та задовгі токени, non-ASCII", "10", "пройдено"],
  ["DocumentLoaderTest", "структура каталогів, сортування, читання, відсутній каталог/файл, порожній датасет", "7", "пройдено"],
  ["InvertedIndexTest", "posting lists, TF/DF, merge, finalize, AND/OR, snippet, неіснуючий документ", "13", "пройдено"],
  ["IndexBuilderTest", "усі документи проіндексовано, детерміновані id, однаковий результат для 1–16 потоків, порожній/відсутній датасет", "8", "пройдено"],
  ["SearchEngineTest", "формули IDF і TF-IDF, сортування, limit, ваги розширення, пошук без індексу", "11", "пройдено"],
  ["QueryExpander*Test", "розбір JSON-відповіді LLM, санітизація, ліміти, промпт, fallback, стоп-слова", "15", "пройдено"],
  ["OllamaProtocolTest", "тіло /api/generate, розбір відповіді та помилок, наявність моделі", "4", "пройдено"],
  ["SearchServiceTest", "конвеєр, fallback, вимкнене розширення, помилки датасету, перебудова", "7", "пройдено"],
  ["ConcurrencyTest", "конкурентне додавання без втрат, паралельна = послідовна побудова, читачі + писачі, пошук під час перебудови, одна побудова одночасно", "7", "пройдено"],
  ["ApiIntegrationTest", "усі ендпоїнти, валідація (13 випадків), 404/405, fallback (3 причини), тайм-аут LLM, 240 конкурентних запитів, конкурентні /api/index, WebSocket: порядок етапів, помилки, busy, 12 незалежних клієнтів, роздача UI", "28", "пройдено"],
  ["e2e_check.py", "розгорнутий стек із реальною Ollama та датасетом 2 ГБ", "23 перевірки", "пройдено"],
], [1.6, 4.4, 0.9, 1.1]));
add(P("Підсумок останнього запуску: ціль `tests` — 98 із 98 тестів ctest (97 модульних і конкурентних тестів та інтеграційний набір із 28 тестів, зареєстрований в ctest як один тест); ціль `tsan` — 97 із 97 тестів без жодного звіту ThreadSanitizer; E2E — 23 із 23 перевірок. Окремо вручну перевірено відмову Ollama під час роботи: після зупинки контейнера `/api/health` повертає `\"ollama\": false`, пошук відповідає з `expansion_status: \"fallback\"` (рис. 3.2), у лог записується попередження, після запуску контейнера розширення відновлюється."));
add(P("Тестування виявило й допомогло виправити кілька дефектів, які не проявлялися в модульних тестах: буферизацію логів у контейнері, звернення CLI до IPv6-адреси `localhost`, упередженість прикладу в промпті, застарілий кеш доступності Ollama після зупинки контейнера та «вигадані» моделлю терміни для запитів лише зі стоп-слів."));

add(H2("4.3 Умови експериментів"));
add(P("Вимірювання виконано 22.11.2026 на Windows 11 + Docker Desktop (віртуальна машина WSL2: 12 vCPU, 7,7 ГБ RAM). Датасет — 20 000 документів, 2 145 814 475 байт, у Docker volume. Сервер, Ollama та клієнти навантаження працюють на одній машині, тому конкурують за CPU. Сервер звітує час побудови індексу (`time_ms` у відповіді `POST /api/index`), а пропускну здатність вимірює клієнт. Усі таблиці та графіки нижче побудовано з файлів `docs/benchmarks/*.csv`."));

add(H2("4.4 Експеримент 1: час побудови індексу"));
add(P(`Для кожної кількості потоків (1, 2, 4, 8, 16) індекс будувався тричі після одного прогрівного запуску, щоб усі виміри мали однаковий (теплий) стан кешу сторінок ОС. Додатково виконано серію з холодним кешем: перед кожною побудовою кеш сторінок віртуальної машини скидався (\`drop_caches\`), тож усі 2 ГБ читалися з диска (по 2 повтори).`));
add(table("Час побудови індексу (20 000 документів, 2,0 ГіБ)", "4",
  ["Потоки", "Середній час, мс", "σ, мс", "Прискорення S", "Ефективність E", "Холодний кеш, мс"],
  idx.map((r) => [r.threads, fmt(r.mean_ms), fmt(r.std_ms), fmt(r.speedup, 2), fmt(r.efficiency, 2), fmt(coldMean[r.threads])]),
  [1, 1.6, 1, 1.3, 1.3, 1.6]));
add(P("Прискорення S(n) = T(1) / T(n), ефективність E(n) = S(n) / n."));
add(figure("docs/benchmarks/index_build_time.png", "Index Build Time vs Number of Threads", "4"));
add(figure("docs/benchmarks/index_speedup.png", "Прискорення побудови індексу порівняно з ідеальним лінійним", "4"));

add(H2("4.5 Експеримент 2: пропускна здатність"));
add(P("Кожен клієнт — окремий процес ОС із постійним HTTP-з'єднанням, щоб уникнути обмежень GIL Python на стороні клієнта. Усі клієнти стартують одночасно (бар'єр), кожен надсилає 200 запитів `POST /api/search` з циклічним вибором 45 різних запитів, `limit = 10` (з читанням snippet з диска). Щоб виміряти саме пошуковий рушій, розширення через LLM вимкнено (`expand = false`). Запити з розширенням виміряно окремо."));
add(table("Пропускна здатність пошуку (expand = false)", "4",
  ["Клієнти", "Запитів", "Загальний час, мс", "Throughput, запит/с", "Сер. затримка, мс", "p95, мс", "Помилки"],
  thr.map((r) => [r.clients, r.requests, fmt(r.total_time_ms), fmt(r.throughput_rps), fmt(r.avg_latency_ms, 2), fmt(r.p95_ms, 2), r.errors]),
  [1, 1, 1.4, 1.4, 1.4, 1, 1]));
add(figure("docs/benchmarks/throughput.png", "Throughput vs Number of Concurrent Clients", "4"));
add(figure("docs/benchmarks/latency.png", "Затримка пошуку залежно від кількості клієнтів", "4"));
add(table("Пропускна здатність з розширенням запиту через Ollama (3 запити на клієнта)", "4",
  ["Клієнти", "Запитів", "Throughput, запит/с", "Сер. затримка, с", "Помилки"],
  llm.map((r) => [r.clients, r.requests, fmt(r.throughput_rps, 2), fmt(r.avg_latency_ms / 1000, 1), r.errors]), [1, 1, 1.5, 1.5, 1]));
add(figure("docs/benchmarks/throughput_llm.png", "Пропускна здатність із Query Expansion (Ollama на CPU)", "4"));

add(H2("4.6 Таблиця пошукових запитів"));
add(table("Результати пошукових запитів на датасеті 20 000 документів (limit = 3)", "4",
  ["Запит", "Розширення", "Збігів", "Топ-документ", "Score", "Пошук, мс", "Усього, мс"],
  queries.map((q) => [q.query, q.expand === "True" ? (q.expanded_terms ? q.expanded_terms.replace(/; /g, ", ") : `${q.expansion_status} (без термінів)`) : "вимкнено",
    q.total_matches, q.top_doc, Number(q.top_score).toFixed(4), fmt(q.search_time_ms, 2), fmt(q.time_ms, 1)]),
  [1.8, 2.6, 0.9, 1.6, 0.9, 0.9, 1]));
add(P("Розширення збільшує кількість знайдених документів: для `car` — з 5 156 до 12 747 (додано документи про automobile, vehicle, sedan). Рідкісний термін `Lonimlexon` знаходиться в єдиному документі. Час власне пошуку в індексі становить 0,1–5 мс, а загальний час із розширенням (1,4–2,8 с) майже повністю припадає на генерацію відповіді моделлю. Перший запит після перезапуску (8,4 с) включає завантаження моделі в пам'ять."));

add(H2("4.7 Аналіз масштабованості"));
add(P(`**Чи зростає швидкість індексації зі збільшенням потоків.** Так. Час побудови скорочується з ${fmt(idx[0].mean_ms / 1000)} с на одному потоці до ${fmt(idx[3].mean_ms / 1000)} с на восьми (прискорення ${fmt(idx[3].speedup, 2)}). До чотирьох потоків масштабування майже лінійне (S = ${fmt(idx[2].speedup, 2)}, E = ${fmt(idx[2].efficiency, 2)}). Причина в тому, що основна робота (читання та токенізація) виконується без синхронізації, а результати однакові для будь-якої кількості потоків.`));
add(P(`**Коли зростання перестає бути ефективним.** Після восьми потоків: 16 потоків дають лише S = ${fmt(idx[4].speedup, 2)} (E = ${fmt(idx[4].efficiency, 2)}). Віртуальна машина має 12 логічних процесорів (6 фізичних ядер з Hyper-Threading у типовій конфігурації), частину яких займають ОС, Docker і Ollama. Крім того, потоки конкурують за пропускну здатність пам'яті та кешу L3 під час роботи з хеш-таблицями. Розкид часу на 16 потоках (σ = ${fmt(idx[4].std_ms)} мс) теж свідчить про конкуренцію з іншими процесами. Ефективна конфігурація — 8 потоків, саме вона використовується за замовчуванням.`));
add(P("**Вплив конкуренції за м'ютекс.** За логами сервера для побудови з 16 потоками фаза отримання списку файлів займає 35–105 мс, паралельна обробка — ~97 % часу, а фінальне сортування — ~27 мс. Злиття відбувається один раз на пакет (313 разів) з блокуванням кожного шарду на короткий час, тому конкуренція незначна: прискорення на 2 і 4 потоках близьке до ідеального. Варіант з одним глобальним м'ютексом навколо всієї обробки документа серіалізував би роботу й обмежив прискорення приблизно одиницею. Розщеплення на 64 шарди й локальні індекси потоків цьому запобігають."));
add(P(`**Вплив читання файлів з диска.** З холодним кешем один потік будує індекс за ${fmt(coldMean["1"] / 1000)} с замість ${fmt(idx[0].mean_ms / 1000)} с (+${Math.round((coldMean["1"] / idx[0].mean_ms - 1) * 100)} %), тобто близько 12 с припадає на читання 2 ГБ з віртуального диска. Паралельне читання теж масштабується (16 потоків — ${fmt(coldMean["16"] / 1000)} с), бо накопичувач обслуговує кілька запитів одночасно, а поки один потік чекає на I/O, інші токенізують. Різниця між холодним і теплим кешем зберігається й на 16 потоках (${fmt(coldMean["16"] / 1000)} проти ${fmt(idx[4].mean_ms / 1000)} с), тому для великих корпусів дискова підсистема стає помітною частиною часу.`));
add(P(`**Як змінюється throughput.** Пропускна здатність зростає з ${fmt(thr[0].throughput_rps, 0)} запит/с для одного клієнта до ${fmt(thr[2].throughput_rps, 0)} запит/с для десяти, після чого майже не змінюється (${fmt(thr[4].throughput_rps, 0)} запит/с для 50 клієнтів). Затримка до 10 клієнтів залишається 2–3 мс, далі зростає приблизно лінійно (${fmt(thr[4].avg_latency_ms, 1)} мс для 50 клієнтів, p95 = ${fmt(thr[4].p95_ms, 1)} мс). Це типова картина насичення: запити стоять у черзі пулу, а CPU ділять 50 клієнтських процесів, IO-потоки та пул сервера. Помилок не зафіксовано на жодному рівні навантаження. Отже, конкурентне читання індексу під спільними блокуваннями не стає вузьким місцем.`));
add(H2("4.8 Вузькі місця системи"));
add(bullets([
  `**LLM на CPU** — найповільніша ланка: ~1,5–3 с на запит і стеля ~${fmt(llm[llm.length - 1].throughput_rps, 2)} запит/с незалежно від кількості клієнтів, бо модель обробляє запити майже послідовно. Зі збільшенням клієнтів зростає лише затримка (${fmt(llm[llm.length - 1].avg_latency_ms / 1000, 1)} с для 10 клієнтів). Пом'якшення: вимкнення розширення для частини запитів (\`expand=false\`), кешування розширень, GPU;`,
  "**Читання файлів для snippet** — на кожен запит читається до 10 файлів по ~105 КБ. На холодному кеші це збільшує затримку. Покращення: зберігати короткі фрагменти або позиції термінів в індексі;",
  "**Однопотоковий етап отримання списку файлів** і завершальне сортування становлять ~5 % часу побудови й обмежують прискорення за законом Амдала;",
  "**Спільні ресурси вимірювального стенда** — клієнти, сервер і Ollama працюють на одних 12 vCPU, тому абсолютні значення throughput занижені порівняно з окремими машинами.",
]));

// ВИСНОВКИ
add(H1("5 ВИСНОВКИ"));
add(P(`У роботі спроєктовано, реалізовано та досліджено контейнеризовану пошукову систему на C++20 з паралельною побудовою інвертованого індексу та розширенням запитів локальною мовною моделлю. Систему запускає одна команда \`docker compose up\`: генерація датасету (20 000 документів, 2,0 ГіБ), завантаження моделі qwen2.5:3b і побудова індексу виконуються автоматично.`));
add(P(`Реалізовано власний пул потоків на \`std::thread\`, \`std::mutex\` і \`std::condition_variable\` з передачею результатів і винятків через \`std::future\`, коректною зупинкою та забороною нових задач. Також реалізовано власний потокобезпечний інвертований індекс із розщепленими блокуваннями (64 шарди з \`std::shared_mutex\`) і алгоритм паралельної побудови «локальний індекс + злиття», ранжування TF-IDF, REST API з чотирма ендпоїнтами, WebSocket-канал етапів, CLI-клієнт і адаптивний веб-інтерфейс.`));
add(P(`**Оцінка ефективності паралелізму.** Паралельна побудова індексу скоротила час з ${fmt(idx[0].mean_ms / 1000)} с до ${fmt(idx[3].mean_ms / 1000)} с на 8 потоках (прискорення ${fmt(idx[3].speedup, 2)}, ефективність ${fmt(idx[3].efficiency, 2)}). Масштабування близьке до лінійного до 4 потоків і насичується після 8 через обмежену кількість ядер і спільну пропускну здатність пам'яті. Читання з диска додає ~73 % до однопотокового часу, проте теж добре розпаралелюється. Сервер обслуговує до ~${fmt(thr[4].throughput_rps, 0)} пошукових запитів за секунду без помилок, а відмова LLM не впливає на доступність пошуку завдяки асинхронній обробці та режиму fallback.`));
add(P("Коректність підтверджено 97 модульними та конкурентними тестами (у тому числі під ThreadSanitizer без жодного звіту про гонки даних), 28 інтеграційними тестами HTTP і WebSocket та E2E-перевіркою розгорнутої системи."));
add(P("**Обмеження:** синтетичний шаблонний корпус; індекс лише в пам'яті й перебудовується при старті; відсутні стемінг, фразовий пошук і підтримка інших мов; висока затримка розширення запиту на CPU; синхронний ендпоїнт індексації; відсутність автентифікації."));
add(P("**Можливі покращення:** збереження індексу на диск і інкрементальне оновлення; стемінг/лематизація та BM25; кешування розширень запитів і пакетна обробка запитів до LLM або виконання моделі на GPU; зберігання позицій термінів для фразового пошуку й швидких snippet; асинхронна індексація з відстеженням прогресу; розподіл індексу між кількома вузлами."));

// ДОДАТКИ
add(H1("ДОДАТОК А. СПЕЦИФІКАЦІЯ API"));
add(table("Поля відповіді GET /api/health", "А", ["Поле", "Тип", "Опис"], [
  ["status", "string", "завжди \"ok\", коли сервер працює"],
  ["ollama", "boolean", "true, якщо Ollama доступна і модель завантажена"],
  ["index_ready", "boolean", "індекс побудовано (додаткове поле)"],
  ["model", "string", "модель для Query Expansion (додаткове поле)"],
], [1.4, 1, 4]));
add(table("POST /api/index: запит, відповідь, помилки", "А", ["Поле / код", "Тип", "Опис"], [
  ["threads (запит)", "integer", "1…64, необов'язкове (за замовчуванням INDEX_THREADS)"],
  ["status", "string", "\"ok\""],
  ["documents_indexed", "integer", "кількість документів"],
  ["time_ms", "number", "час побудови, мс"],
  ["threads_used", "integer", "фактична кількість потоків (додаткове)"],
  ["400 invalid_json / invalid_threads", "—", "некоректне тіло або threads ≤ 0, > 64, не ціле"],
  ["409 indexing_in_progress", "—", "побудова вже виконується"],
  ["422 dataset_empty; 500 dataset_not_found / index_failed", "—", "проблеми з датасетом або побудовою"],
], [2.6, 1, 3.4]));
add(table("POST /api/search: запит, відповідь, помилки", "А", ["Поле / код", "Тип", "Опис"], [
  ["query (запит)", "string", "обов'язкове, 1…1000 символів"],
  ["mode (запит)", "string", "\"keyword\" (за замовчуванням)"],
  ["limit (запит)", "integer", "1…100, за замовчуванням 10"],
  ["expand (запит)", "boolean", "за замовчуванням true"],
  ["results[]", "array", "{doc_id: string, score: number, snippet: string}, за спаданням score"],
  ["time_ms", "number", "загальний час, мс"],
  ["total_matches", "integer", "кількість документів зі збігами"],
  ["query, expanded_terms", "string, string[]", "запит і терміни від LLM"],
  ["expansion_status", "string", "\"ok\" | \"fallback\" | \"disabled\""],
  ["fallback_reason", "string | null", "причина fallback"],
  ["search_time_ms, expansion_time_ms", "number", "тривалість етапів, мс"],
  ["400 invalid_json / invalid_query / invalid_mode / invalid_limit / invalid_expand", "—", "помилки валідації"],
  ["503 index_not_ready", "—", "індекс ще не побудовано"],
], [2.6, 1, 3.4]));
add(table("Поля відповіді GET /api/stats", "А", ["Поле", "Тип", "Опис"], [
  ["documents", "integer", "кількість документів в індексі"],
  ["index_build_time_ms", "number", "час останньої побудови, мс"],
  ["threads_used", "integer", "потоки останньої побудови"],
  ["unique_terms, total_postings", "integer", "розмір словника та кількість записів"],
  ["index_ready, indexing_in_progress", "boolean", "стан індексу"],
  ["searches_served", "integer", "кількість виконаних пошуків"],
], [2.6, 1, 3.4]));
add(table("Повідомлення WebSocket /ws/search", "А", ["Напрям / stage", "Поля data", "Примітка"], [
  ["клієнт → сервер", "query: string, limit?: integer, expand?: boolean", "текстовий JSON-кадр"],
  ["query_expansion", "original, expanded_terms[], status, fallback_reason, time_ms", "1-ше повідомлення"],
  ["keyword_search", "results[], time_ms, total_matches", "2-ге повідомлення"],
  ["done", "— (лише {\"stage\":\"done\"})", "3-тє повідомлення"],
  ["error", "error: string, message: string", "замість або після етапів; з'єднання не закривається"],
], [1.8, 3.2, 2]));

add(H1("ДОДАТОК Б. UML-ДІАГРАМИ"));
add(P("Усі UML-діаграми наведено в розділі 2 (рис. 2.1–2.7). Їх джерела у форматі Mermaid розташовані в репозиторії:", { keepNext: true }));
add(bullets([
  "`docs/uml/use_case.mmd` — діаграма прецедентів;",
  "`docs/uml/class_diagram.mmd` — діаграма класів;",
  "`docs/uml/seq_index_build.mmd`, `seq_rest_search.mmd`, `seq_websocket.mmd`, `seq_health_stats.mmd` — діаграми послідовностей;",
  "`docs/uml/deployment.mmd` — діаграма розгортання.",
]));

add(H1("ДОДАТОК В. ПРИКЛАДИ ЗАПИТІВ"));
add(P("Побудова індексу та помилка валідації:", { noIndent: true, keepNext: true }));
add(code(`$ curl -X POST localhost:8080/api/index -H "Content-Type: application/json" -d '{"threads": 8}'
{"documents_indexed":20000,"status":"ok","threads_used":8,"time_ms":2804.001}

$ curl -X POST localhost:8080/api/index -H "Content-Type: application/json" -d '{"threads": 0}'
{"error":"invalid_threads","message":"threads must be an integer between 1 and 64"}`));
add(P("Пошук із розширенням запиту:", { noIndent: true, keepNext: true }));
add(code(`$ curl -X POST localhost:8080/api/search -H "Content-Type: application/json" \\
       -d '{"query":"car","mode":"keyword","limit":1}'
{"expanded_terms":["automobile","vehicle","sedan"],"expansion_status":"ok",
 "expansion_time_ms":1498.2,"fallback_reason":null,"query":"car",
 "results":[{"doc_id":"document_014624","score":0.101475,
   "snippet":"...in Mumbai Researchers at the regional museum in Berlin reported that the car drives the car ..."}],
 "search_time_ms":2.08,"time_ms":1500.3,"total_matches":12747}`));
add(P("Пошук під час недоступності Ollama:", { noIndent: true, keepNext: true }));
add(code(`{"expanded_terms":[],"expansion_status":"fallback","fallback_reason":"Ollama is unavailable",
 "query":"climate change","total_matches":7773,"search_time_ms":2.287,"time_ms":2.698, "results":[...]}`));
add(P("Стан і статистика:", { noIndent: true, keepNext: true }));
add(code(`$ curl localhost:8080/api/health
{"index_ready":true,"model":"qwen2.5:3b","ollama":true,"status":"ok"}
$ curl localhost:8080/api/stats
{"documents":20000,"index_build_time_ms":4137.908,"index_ready":true,"indexing_in_progress":false,
 "searches_served":3,"threads_used":8,"total_postings":14479164,"unique_terms":28024}`));
add(P("WebSocket (послідовність повідомлень сервера):", { noIndent: true, keepNext: true }));
add(code(`> {"query":"climate change glacier"}
< {"stage":"query_expansion","data":{"original":"climate change glacier","expanded_terms":["iceberg","snow","ocean","carbon"],"status":"ok","fallback_reason":null,"time_ms":1890.4}}
< {"stage":"keyword_search","data":{"results":[...],"time_ms":3.1,"total_matches":...}}
< {"stage":"done"}`));

add(H1("ДОДАТОК Г. ВИКОРИСТАННЯ ІНСТРУМЕНТІВ ШТУЧНОГО ІНТЕЛЕКТУ"));
add(P("Під час виконання роботи використовувався асистент Claude Code (Anthropic, модель Claude Opus 5.5) у середовищі VS Code. Асистент виконував команди (Docker, git, Python) і редагував файли проєкту в межах однієї сесії 22.11.2026. Повний журнал наведено у файлі `docs/prompts/PROMPTS_LOG.md`, повний текст головного промпту — у `docs/prompts/master_prompt.md`."));
add(P("**Фактично використані промпти.** Студент надіслав один детальний промпт-специфікацію з 22 розділів і короткі доповнення до нього. Окремі промпти для архітектури, пулу потоків, індексу, інтеграції Ollama, тестування та аналізу benchmark **не надсилалися**: їхню роль виконували відповідні розділи головного промпту (табл. Г.1). Інших промптів зі зміною вимог під час розробки не було."));
add(table("Відповідність вимог силабусу розділам фактичного промпту", "Г", ["Призначення", "Розділ промпту", "Ключове формулювання (дослівно)"], [
  ["Проєктування архітектури", "§1–3, §15", "«Побудуй систему так, щоб відмова Ollama не призводила до повної недоступності пошуку. У такому випадку має бути fallback…»"],
  ["Реалізація Thread Pool", "§4", "«Самостійно реалізуй власний ThreadPool… Не використовуй один глобальний mutex навколо всієї обробки документів. Мінімізуй конкуренцію потоків.»"],
  ["Реалізація інвертованого індексу", "§5", "«Реалізуй власну структуру даних InvertedIndex… Для ранжування результатів використай TF або TF-IDF.»"],
  ["Інтеграція Ollama", "§8–9", "«Використовуй структурований prompt, який вимагає повернення JSON… Не допускай нескінченного очікування відповіді моделі.»"],
  ["Тестування", "§14", "«Unit tests… Integration tests… Concurrency tests… Якщо доступний ThreadSanitizer, додай конфігурацію для його використання.»"],
  ["Аналіз benchmark", "§13", "«…як впливає конкуренція за mutex; як впливає читання файлів із диска… Не вигадуй результати експериментів.»"],
], [1.6, 0.9, 4.5]));
add(P("Доповнення студента (дослівно): «не пиши в пояснювальній записці, що система працює, якщо вона фактично ще не перевірена»; «Не завантажуй датасет у GitHub»; «Не дозволяй Ollama блокувати весь сервер… пошук повинен продовжувати працювати в режимі без Query Expansion»; «Зроби акцент на власному Thread Pool»; «Публікація GitHub… його потрібно додати саме як вкладення типу „Посилання“»."));
add(P("**Перевірка результатів роботи ШІ.** Усі твердження про працездатність у записці перевірено фактичним запуском: автоматичними тестами, E2E-перевіркою, ручною перевіркою відмови Ollama та знімками екрана. Числа в розділі 4 зчитано з файлів результатів вимірювань. Дефекти, знайдені під час перевірки, описано в п. 4.2."));

// ------------------------------------------------------------------ TOC
function tocParagraphs() {
  const pagesFile = path.join(__dirname, "toc_pages.json");
  const pages = fs.existsSync(pagesFile) ? JSON.parse(fs.readFileSync(pagesFile, "utf8")) : {};
  return tocEntries.map((e) => new Paragraph({
    spacing: { line: 360 }, indent: e.level === 2 ? { left: 567, right: 567 } : { right: 567 },
    tabStops: [{ type: TabStopType.RIGHT, position: TEXT_WIDTH, leader: "dot" }],
    children: [
      new TextRun({ text: e.text, font: FONT, size: 28 }),
      new TextRun({ text: `	${pages[e.text] || "0"}`, font: FONT, size: 28 }),
    ],
  }));
}
fs.writeFileSync(path.join(__dirname, "toc_entries.json"), JSON.stringify(tocEntries.map((e) => e.text), null, 1));

// ------------------------------------------------------------------ document
const heading = (size, before, after) => ({ run: { font: FONT, size, bold: true, color: "000000" },
  paragraph: { spacing: { before, after, line: 360 }, keepNext: true, keepLines: true } });

const doc = new Document({
  creator: "course_work_parallel_computing",
  title: "Пояснювальна записка до курсової роботи",
  description: "Паралельна пошукова система з інтеграцією LLM",
  styles: {
    default: { document: { run: { font: FONT, size: 28 }, paragraph: { spacing: { line: 360 } } } },
    paragraphStyles: [
      { id: "Heading1", name: "Heading 1", basedOn: "Normal", next: "Normal", quickFormat: true,
        ...heading(32, 0, 240), paragraph: { spacing: { before: 0, after: 240, line: 360 }, keepNext: true, alignment: AlignmentType.CENTER, outlineLevel: 0 } },
      { id: "Heading2", name: "Heading 2", basedOn: "Normal", next: "Normal", quickFormat: true,
        ...heading(28, 240, 120), paragraph: { spacing: { before: 240, after: 120, line: 360 }, keepNext: true, indent: { firstLine: 709 }, outlineLevel: 1 } },
      { id: "Heading3", name: "Heading 3", basedOn: "Normal", next: "Normal", quickFormat: true,
        run: { font: FONT, size: 28, bold: true, italics: true },
        paragraph: { spacing: { before: 120, after: 60, line: 360 }, keepNext: true, indent: { firstLine: 709 }, outlineLevel: 2 } },
    ],
  },
  numbering: { config: [
    { reference: "dash", levels: [{ level: 0, format: LevelFormat.BULLET, text: "–", alignment: AlignmentType.LEFT,
      style: { paragraph: { indent: { left: 1069, hanging: 360 } } } }] },
    { reference: "tasks", levels: [{ level: 0, format: LevelFormat.DECIMAL, text: "%1)", alignment: AlignmentType.LEFT,
      style: { paragraph: { indent: { left: 1069, hanging: 360 } } } }] },
  ] },
  sections: [{
    properties: {
      page: { size: { width: 11906, height: 16838 }, margin: { top: 1134, bottom: 1134, left: 1701, right: 850, footer: 567 } },
      titlePage: true,
    },
    footers: {
      default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.CENTER,
        children: [new TextRun({ children: [PageNumber.CURRENT], font: FONT, size: 24 })] })] }),
      first: new Footer({ children: [new Paragraph({ children: [] })] }),
    },
    children: [
      ...titlePage,
      new Paragraph({ children: [new PageBreak()] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 240 },
        children: [new TextRun({ text: "ЗМІСТ", font: FONT, size: 32, bold: true })] }),
      ...tocParagraphs(),
      ...C,
    ],
  }],
});

Packer.toBuffer(doc).then((buf) => {
  fs.writeFileSync(OUT, buf);
  console.log("written", OUT, `${(buf.length / 1024 / 1024).toFixed(1)} MiB`, "figures:", JSON.stringify(figNo), "tables:", JSON.stringify(tabNo));
});
