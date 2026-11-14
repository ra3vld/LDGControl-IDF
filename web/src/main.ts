export type State = {
    boot_id: string;
    source_token: string;
    sequence: number;
    uptime_ms: number;
    connected: boolean;
    busy: boolean;
    source: string;
    fwd_power: number | null;
    ref_power: number | null;
    swr: number | null;
    band: string;
    mode: number;
    antenna: number;
    meter_age_ms: number;
    scale_watts?: number;
    logarithmic?: boolean;
    radio?: {
        state: string;
        tx_frequency_hz: number | null;
        band: string;
        rx_tx: string;
    };
    route?: {
        stage: string;
        partial: boolean;
        result: string;
    };
    external_antenna?: {
        requested: number;
        confirmed: number;
        result: string;
        evidence: number;
    };
};
export type Context = {
    api: typeof api;
    message: typeof message;
    t: typeof t;
    state: () => State | null;
    caps: Record<string, unknown>;
};
const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const texts: Record<string, [
    string,
    string
]> = { work: ['Управление', 'Control'], settings: ['Настройки', 'Settings'], service: ['Обслуживание', 'Service'], external: ['Внешний переключатель', 'External switch'], port: ['Порт', 'Port'], select: ['Выбрать', 'Select'], history: ['История · 30 минут', 'History · 30 minutes'], txOnly: ['Только время передачи', 'TX time only'], loadingCharts: ['Графики загружаются в фоне…', 'Charts are loading in the background…'], retry: ['Повторить', 'Retry'], optional: ['Каждая функция настраивается отдельно. Ненужные подключения можно оставить выключенными.', 'Configure each feature independently. Unused connections can stay disabled.'], updates: ['Обновления', 'Updates'], checkUpdates: ['Проверить GitHub', 'Check GitHub'], manifest: ['Метаданные релиза (JSON)', 'Release manifest (JSON)'], firmware: ['Прошивка (.bin)', 'Firmware (.bin)'], installFile: ['Установить файл', 'Install file'], cancel: ['Отменить', 'Cancel'], diagnostics: ['Диагностика', 'Diagnostics'], refresh: ['Обновить', 'Refresh'], mqttReconnect: ['Переподключить MQTT', 'Reconnect MQTT'], restart: ['Перезапустить устройство', 'Restart device'], save: ['Сохранить', 'Save'], loading: ['Загрузка…', 'Loading…'], saved: ['Сохранено', 'Saved'], savedRestart: ['Сохранено. Перезапустите устройство для применения сети или UART.', 'Saved. Restart to apply network or UART changes.'], online: ['На связи', 'Connected'], offline: ['Нет свежих данных · повторяем подключение', 'No fresh data · reconnecting'], accepted: ['Команда принята; ожидаем результат…', 'Command accepted; waiting for result…'], success: ['Выполнено', 'Completed'], failed: ['Операция не выполнена. Проверьте подключение и повторите при необходимости.', 'Operation failed. Check the connection and retry if needed.'], busy: ['Устройство занято. Повторите после завершения текущей операции.', 'Device is busy. Retry after the current operation.'], expired: ['Команда устарела и не была запущена. Обновите состояние и повторите.', 'The command expired before starting. Refresh state and retry.'], target_changed: ['Получатель изменился. Проверьте выбранный контроллер.', 'Recipient changed. Check the selected controller.'], unsafe: ['Переключение ожидает свежего подтверждения отсутствия RF.', 'Switching requires fresh confirmation of zero RF.'], unavailable: ['Функция недоступна. Проверьте её настройки и подключение.', 'Feature unavailable. Check its settings and connection.'], invalid_request: ['Проверьте введённые значения.', 'Check the entered values.'], invalid_json: ['Некорректные данные запроса.', 'Invalid request data.'], config_conflict: ['Настройки изменены в другом окне. Ваш ввод сохранён; загрузите актуальную версию для сравнения.', 'Settings changed in another window. Your input is preserved; load the current revision to compare.'], authentication_required: ['Требуется вход. Откройте страницу устройства заново; введённые данные сохранены.', 'Sign in again by opening the device page; entered values are preserved.'], unknown_operation: ['Результат неизвестен. Проверьте состояние тюнера перед новой командой.', 'Result unknown. Check tuner state before issuing another command.'], unknown: ['Результат неизвестен. Проверьте состояние тюнера перед новой командой.', 'Result unknown. Check tuner state before issuing another command.'], timeout: ['Ответ оборудования не получен вовремя. Проверьте состояние перед повтором.', 'Equipment reply timed out. Check state before retrying.'], network: ['Нет ответа. Проверьте соединение; введённые данные сохранены.', 'No response. Check the connection; entered values are preserved.'], noChange: ['Не переключать', 'Leave unchanged'], enabled: ['Включено', 'Enabled'], host: ['Адрес устройства', 'Host'], username: ['Имя пользователя', 'Username'], password: ['Пароль (пусто — сохранить)', 'Password (blank keeps current)'], networkTitle: ['Wi-Fi и сеть', 'Wi-Fi and network'], mqttTitle: ['MQTT / Home Assistant', 'MQTT / Home Assistant'], radioTitle: ['Трансивер · TCI', 'Transceiver · TCI'], remoteTitle: ['Удалённый тюнер', 'Remote tuner'], tuningTitle: ['CONTROLLED', 'CONTROLLED'], antennasTitle: ['Антенны по диапазонам', 'Antennas by band'], displayTitle: ['Экран', 'Display'], meterTitle: ['Измерители', 'Meters'], deviceTitle: ['Устройство и доступ', 'Device and access'], newer: ['Доступно обновление', 'Update available'], current: ['Новых стабильных релизов нет', 'No newer stable release'], checkFailed: ['Не удалось проверить GitHub. Можно повторить или установить файл.', 'GitHub check failed. Retry or install a file.'], stage: ['Этап', 'Stage'], cancelled: ['Отменено', 'Cancelled'], loadCurrent: ['Загрузить актуальные значения', 'Load current values'], reapply: ['Сохранить мой вариант', 'Save my version'], restartConfirm: ['Перезапустить устройство сейчас?', 'Restart the device now?'] };
Object.assign(texts, { "Static IP": ["Статический IP", "Static IP"], "Netmask": ["Маска сети", "Netmask"], "Gateway": ["Шлюз", "Gateway"], "Receiver": ["Приёмник", "Receiver"], "AUTO fallback": ["AUTO при сохраняющемся высоком КСВ", "AUTO fallback"], "SWR threshold": ["Порог КСВ", "SWR threshold"], "ANT1 / ANT2 auto": ["Автовыбор ANT1 / ANT2", "ANT1 / ANT2 auto"], "External auto": ["Автовыбор внешнего порта", "External auto"], "Simulated switch": ["Симулятор переключателя", "Simulated switch"], "ANT1 name": ["Название ANT1", "ANT1 name"], "ANT2 name": ["Название ANT2", "ANT2 name"], "Auto brightness": ["Автояркость (датчик Sunton)", "Auto brightness (Sunton sensor)"], "Brightness %": ["Яркость, %", "Brightness %"], "Minimum %": ["Минимум, %", "Minimum %"], "Maximum %": ["Максимум, %", "Maximum %"], "Dark ADC": ["АЦП в темноте", "Dark ADC"], "Bright ADC": ["АЦП на свету", "Bright ADC"], "Rotation 0 / 180": ["Поворот 0 / 180°", "Rotation 0 / 180"], "Scale W": ["Предел шкалы, Вт", "Scale W"], "Logarithmic": ["Логарифмическая шкала", "Logarithmic"], "PSU voltage": ["Напряжение питания тюнера, В", "PSU voltage"], "duplicate_conflict": ["Этот ID уже использован с другой командой. Обновите состояние.", "This ID belongs to another request. Refresh state."], "invalid_manifest": ["Метаданные не подходят этой плате. Выберите комплект для нужной платы.", "Manifest does not match this board. Select the matching package."], "signature_invalid": ["Подпись не прошла проверку. Выберите релиз, подписанный ключом этой прошивки.", "Signature rejected. Select a release signed by this firmware's key."], "image_verification_failed": ["Образ повреждён. Действующая прошивка сохранена; загрузите файл заново.", "Image verification failed. Current firmware is intact; upload again."], "update_idle_timeout": ["Обновление отменено по тайм-ауту. Начните заново.", "Update timed out. Start again."], "memory_budget": ["Недостаточно свободной памяти. Повторите после отключения лишнего клиента.", "Not enough free memory. Disconnect an extra client and retry."], "network_cleanup_timeout": ["Соединения не завершились вовремя. Повторите обновление.", "Connection cleanup timed out. Retry the update."], "download_failed": ["Не удалось скачать образ. Повторите или загрузите файл.", "Image download failed. Retry or upload a file."], "receiving": ["Загрузка", "Receiving"], "preparing": ["Подготовка", "Preparing"], "waiting": ["Ожидание завершения команды и передачи", "Waiting for operation and RF to finish"], "verifying": ["Проверка", "Verifying"], "ready": ["Готово, перезапуск", "Ready, restarting"] });
Object.assign(texts, { partial: ["Маршрут выполнен частично", "Route partly applied"], waiting_rf: ["Ожидание нулевой мощности", "Waiting for zero RF"], waiting_band: ["Ожидание диапазона", "Waiting for band"], complete: ["Маршрут выполнен", "Route complete"], switching: ["Переключение", "Switching"], pending: ["Ожидание", "Pending"] });
Object.assign(texts, {
    connecting: ['Подключение…', 'Connecting…'],
    local: ['Локальный', 'Local'],
    remote: ['Удалённый', 'Remote'],
    closeNotification: ['Закрыть уведомление', 'Close notification'],
    switchToRussian: ['Переключить на русский', 'Switch to Russian'],
    switchToEnglish: ['Переключить на английский', 'Switch to English'],
    installRelease: ['Установить релиз', 'Install release'],
    minutes30: ['−30 мин', '−30 min'],
    'Home Assistant discovery': ['Обнаружение Home Assistant', 'Home Assistant discovery']
});
type Language = 'en' | 'ru';
function preferredLanguage(): Language {
    try { return localStorage.getItem('ldg.language') === 'ru' ? 'ru' : 'en'; }
    catch { return 'en'; }
}
let language = preferredLanguage();
export function t(key: string): string { return texts[key]?.[language === 'ru' ? 0 : 1] ?? key; }
function translate() {
    document.documentElement.lang = language;
    document.querySelectorAll<HTMLElement>('[data-i18n]').forEach(el => el.textContent = t(el.dataset.i18n!) + (el.dataset.i18nSuffix ?? ''));
    document.querySelectorAll<HTMLElement>('[data-i18n-aria-label]').forEach(el => el.setAttribute('aria-label', t(el.dataset.i18nAriaLabel!)));
    $('language').textContent = language === 'ru' ? 'EN' : 'RU';
    $('language').setAttribute('aria-label', t(language === 'ru' ? 'switchToEnglish' : 'switchToRussian'));
    $('language').title = $('language').getAttribute('aria-label')!;
}
$('language').onclick = () => {
    language = language === 'ru' ? 'en' : 'ru';
    try { localStorage.setItem('ldg.language', language); } catch { }
    translate();
    document.dispatchEvent(new Event('ldg-language-change'));
};
translate();
let toastTimer: number | undefined, toastRemaining = 8000, toastStart = 0;
const toast = $('toast');
function hideToast() { clearTimeout(toastTimer); toast.hidden = true; }
function expireToast() { clearTimeout(toastTimer); toastStart = performance.now(); toastTimer = window.setTimeout(hideToast, toastRemaining); }
function pauseToast() { clearTimeout(toastTimer); toastRemaining = Math.max(0, toastRemaining - (performance.now() - toastStart)); }
function resumeToast() { if (!toast.matches(':hover') && !toast.contains(document.activeElement))
    expireToast(); }
toast.onmouseenter = pauseToast;
toast.onmouseleave = resumeToast;
toast.addEventListener('focusin', pauseToast);
toast.addEventListener('focusout', () => setTimeout(resumeToast, 0));
toast.querySelector('button')!.onclick = hideToast;
function rememberTranslation(target: HTMLElement, text: string) {
    delete target.dataset.i18n;
    delete target.dataset.i18nSuffix;
    for (const [key, values] of Object.entries(texts)) {
        if (values.includes(text)) { target.dataset.i18n = key; return; }
    }
    for (const [key, values] of Object.entries(texts)) {
        const prefix = values.find(value => text.startsWith(value + ': ') || text.startsWith(value + ' · ') || text.startsWith(value + ' '));
        if (prefix) {
            target.dataset.i18n = key;
            target.dataset.i18nSuffix = text.slice(prefix.length);
            return;
        }
    }
}
export function message(target: HTMLElement, text: string, error = false) {
    rememberTranslation(target, text);
    target.textContent = text;
    target.classList.toggle('error', error);
    if (error) {
        const label = toast.querySelector<HTMLElement>('span')!;
        rememberTranslation(label, text);
        label.textContent = text;
        toast.hidden = false;
        toastRemaining = 8000;
        expireToast();
    }
}
export async function api(path: string, body?: unknown, method?: string): Promise<any> { let response: Response; try {
    response = await fetch('/api/v2/' + path, { method: method ?? (body === undefined ? 'GET' : 'POST'), headers: body === undefined ? {} : { 'Content-Type': 'application/json' }, body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(5000) });
}
catch {
    throw new Error('network');
} let data: any; try {
    data = await response.json();
}
catch {
    throw new Error('network');
} if (!response.ok)
    throw new Error(data.error ?? 'failed'); return data; }
let state: State | null = null, lastStateAt = 0, generation = 0;
const listeners = new Set<(s: State, at: number) => void>();
export function subscribe(fn: (s: State, at: number) => void) { listeners.add(fn); }
function receive(s: State) { if (!s.boot_id || !s.source_token)
    return; if (state && s.boot_id === state.boot_id && s.source_token === state.source_token && ((s.sequence - state.sequence) >>> 0) > 0x7fffffff)
    return; if (state && (s.boot_id !== state.boot_id || s.source_token !== state.source_token))
    generation++; state = s; lastStateAt = performance.now(); render(); for (const callback of listeners)
    callback(s, Date.now()); }
function render() { const fresh = state && performance.now() - lastStateAt < 3000 && state.connected; const s = state; for (const [id, value] of [['fwd', s?.fwd_power], ['ref', s?.ref_power], ['swr', s?.swr]] as const)
    $(id).textContent = fresh && value != null ? value.toFixed(id === 'swr' ? 2 : 1) : '—'; $('connection').textContent = s?.busy ? t('accepted') : t(fresh ? 'online' : 'offline'); $('connection').classList.toggle('stale', !fresh); $('source').textContent = s?.source ? t(s.source) : ''; $('band').textContent = s?.band ?? '—'; $('mode').textContent = ['—', 'AUTO', 'MANUAL', 'BYPASS', 'CONTROLLED'][s?.mode ?? 0]; $('antenna').textContent = 'ANT ' + (s?.antenna || '—'); $('radio').textContent = s?.radio?.tx_frequency_hz ? `TCI ${(s.radio.tx_frequency_hz / 1e6).toFixed(5)} MHz` : 'TCI —'; for (const [id, value] of [['fwd-meter', s?.fwd_power], ['ref-meter', s?.ref_power]] as const) {
    $<HTMLMeterElement>(id).max = 1;
    const v = Math.min(1, Math.max(0, (value ?? 0) / (s?.scale_watts ?? 1000)));
    $<HTMLMeterElement>(id).value = fresh ? (s?.logarithmic ? Math.log10(1 + 9 * v) : v) : 0;
} $('swr').dataset.level = !fresh || s?.swr == null ? 'unknown' : s.swr > 2 ? 'danger' : s.swr > 1.5 ? 'warning' : 'normal';
    $('mode').dataset.mode = fresh ? String(s?.mode ?? 0) : '0';
    for (const [command, selected] of [['antenna_1', s?.antenna === 1], ['antenna_2', s?.antenna === 2], ['auto', s?.mode === 1], ['manual', s?.mode === 2], ['bypass', s?.mode === 3], ['controlled', s?.mode === 4]] as const)
        document.querySelector(`[data-command="${command}"]`)?.setAttribute('aria-pressed', String(Boolean(fresh && selected)));
    $('external-state').textContent = s?.external_antenna ? `${s.external_antenna.requested || '—'} → ${s.external_antenna.confirmed || '—'} · ${t(s.external_antenna.result)}${s.route?.stage !== 'disabled' ? ' · ' + t(s.route?.stage ?? '') + (s.route?.partial ? ' · ' + t('partial') : '') : ''}` : ''; }
document.addEventListener('ldg-language-change', render);
setInterval(render, 500);
let socket: WebSocket | null = null, socketEpoch = 0;
async function connect() { const epoch = ++socketEpoch; try {
    const token = await api('ws-token');
    if (epoch !== socketEpoch)
        return;
    socket = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/api/v2/ws?t=${encodeURIComponent(token.token)}`);
    socket.onmessage = e => { if (epoch !== socketEpoch)
        return; try {
        receive(JSON.parse(e.data));
    }
    catch { } };
    socket.onclose = () => { if (epoch === socketEpoch)
        setTimeout(connect, 2000); };
    socket.onerror = () => socket?.close();
}
catch {
    setTimeout(connect, 3000);
} }
let polling = false;
setInterval(async () => { if (polling || performance.now() - lastStateAt < 1500)
    return; polling = true; const epoch = generation; try {
    const s = await api('state');
    if (epoch === generation)
        receive(s);
}
catch { }
finally {
    polling = false;
} }, 1000);
void api('state').then(receive).catch(() => { });
void connect();
function uuid() { if (crypto.randomUUID)
    return crypto.randomUUID(); const a = crypto.getRandomValues(new Uint8Array(16)); a[6] = (a[6] & 15) | 64; a[8] = (a[8] & 63) | 128; const s = Array.from(a, b => b.toString(16).padStart(2, '0')).join(''); return `${s.slice(0, 8)}-${s.slice(8, 12)}-${s.slice(12, 16)}-${s.slice(16, 20)}-${s.slice(20)}`; }
async function command(code: string, button: HTMLButtonElement, target: HTMLElement, port?: number) {
    if (!state || performance.now() - lastStateAt > 3000) {
        message(target, t('unavailable'), true);
        return;
    }
    if (code === 'bypass' && state.mode === 3)
        code = 'auto';
    const id = uuid(), request = { command: code, request_id: id, source_token: state.source_token, expires_uptime_ms: (state.uptime_ms + Math.floor(performance.now() - lastStateAt) + 1900) >>> 0, port };
    button.disabled = true;
    message(target, t('accepted'));
    try {
        try {
            await api('commands', request);
        }
        catch (e) {
            if ((e as Error).message !== 'network')
                throw e;
        }
        const start = performance.now();
        for (;;) {
            await new Promise(r => setTimeout(r, 350));
            const operation = await api('commands/' + id);
            if (operation.state === 'done') {
                message(target, t(operation.result), operation.result !== 'success');
                break;
            }
            if (performance.now() - start > 30000)
                throw new Error('unknown');
        }
    }
    catch (e) {
        message(target, t((e as Error).message), true);
    }
    finally {
        button.disabled = false;
    }
}
document.querySelectorAll<HTMLButtonElement>('[data-command]').forEach(button => button.onclick = () => void command(button.dataset.command!, button, $('command-result')));
$<HTMLFormElement>('external-form').onsubmit = e => { e.preventDefault(); const form = e.currentTarget as HTMLFormElement; void command('external_antenna', form.querySelector('button')!, form.querySelector('.action-result')!, Number(new FormData(form).get('port'))); };
type Field = [
    string,
    string,
    'text' | 'password' | 'number' | 'checkbox',
    number?,
    number?
];
const sections: Record<string, Field[]> = { network: [['ssid', 'SSID', 'text'], ['password', 'password', 'password'], ['static', 'Static IP', 'checkbox'], ['ip', 'IP', 'text'], ['netmask', 'Netmask', 'text'], ['gateway', 'Gateway', 'text'], ['dns', 'DNS', 'text']], mqtt: [['enabled', 'enabled', 'checkbox'], ['host', 'host', 'text'], ['port', 'port', 'number', 1, 65535], ['username', 'username', 'text'], ['password', 'password', 'password'], ['discovery', 'Home Assistant discovery', 'checkbox']], radio: [['enabled', 'enabled', 'checkbox'], ['host', 'host', 'text'], ['port', 'port', 'number', 1, 65535], ['receiver', 'Receiver', 'number', 0, 15]], remote: [['enabled', 'enabled', 'checkbox'], ['host', 'host', 'text']], tuning: [['auto_fallback', 'AUTO fallback', 'checkbox'], ['swr_threshold', 'SWR threshold', 'number', 1.1, 10]], antennas: [['enabled', 'ANT1 / ANT2 auto', 'checkbox'], ['external_enabled', 'External auto', 'checkbox'], ['simulate', 'Simulated switch', 'checkbox'], ['ant1_name', 'ANT1 name', 'text'], ['ant2_name', 'ANT2 name', 'text']], display: [['automatic', 'Auto brightness', 'checkbox'], ['brightness', 'Brightness %', 'number', 1, 100], ['minimum', 'Minimum %', 'number', 1, 100], ['maximum', 'Maximum %', 'number', 1, 100], ['dark_adc', 'Dark ADC', 'number', 0, 4095], ['bright_adc', 'Bright ADC', 'number', 0, 4095], ['rotation', 'Rotation 0 / 180', 'number', 0, 180]], meter: [['watts', 'Scale W', 'number', 1, 1000], ['logarithmic', 'Logarithmic', 'checkbox'], ['psu_voltage', 'PSU voltage', 'number', 8, 18]], device: [['username', 'username', 'text'], ['password', 'password', 'password'], ['uart_rx', 'UART RX', 'number', -1, 48], ['uart_tx', 'UART TX', 'number', -1, 48]] };
const bands = ['160m', '80m', '60m', '40m', '30m', '20m', '17m', '15m', '12m', '10m', '6m'];
for (const [name, fields] of Object.entries(sections)) {
    const details = document.createElement('details'), summary = document.createElement('summary');
    summary.dataset.i18n = name + 'Title';
    summary.textContent = t(name + 'Title');
    details.append(summary);
    const form = document.createElement('form');
    form.hidden = true;
    details.append(form);
    const result = document.createElement('p');
    result.className = 'action-result';
    result.setAttribute('role', 'status');
    details.append(result);
    let loaded = false, loading = false, revision = 0;
    for (const [key, title, type, min, max] of fields) {
        const label = document.createElement('label'), span = document.createElement('span'), input = document.createElement('input');
        span.dataset.i18n = title;
        span.textContent = t(title);
        input.name = key;
        input.type = type;
        if (min !== undefined)
            input.min = String(min);
        if (max !== undefined)
            input.max = String(max);
        if (type === 'number')
            input.step = key === 'swr_threshold' || key === 'psu_voltage' ? '0.1' : key === 'rotation' ? '180' : '1';
        if (type === 'password')
            input.autocomplete = 'new-password';
        label.append(span, input);
        form.append(label);
    }
    if (name === 'antennas')
        for (let i = 0; i < 11; i++) {
            const row = document.createElement('div');
            row.className = 'band-row';
            const title = document.createElement('span');
            title.textContent = bands[i];
            row.append(title);
            for (const kind of ['ldg', 'external']) {
                const select = document.createElement('select');
                select.name = `band_${i}_${kind}`;
                select.setAttribute('aria-label', bands[i] + ' ' + kind);
                for (let p = 0; p <= (kind === 'ldg' ? 2 : 16); p++) {
                    const o = document.createElement('option');
                    o.value = String(p);
                    o.textContent = p ? `${kind === 'ldg' ? 'ANT' : 'EXT'} ${p}` : t('noChange');
                    if (!p) o.dataset.i18n = 'noChange';
                    select.append(o);
                }
                row.append(select);
            }
            form.append(row);
        }
    const save = document.createElement('button');
    save.type = 'submit';
    save.dataset.i18n = 'save';
    save.textContent = t('save');
    form.append(save);
    function fill(data: any) { revision = data.revision; for (const [key, , type] of fields) {
        const input = form.elements.namedItem(key) as HTMLInputElement;
        if (type === 'checkbox')
            input.checked = !!data[key];
        else
            input.value = type === 'password' ? '' : String(data[key] ?? '');
    } if (name === 'antennas')
        data.bands?.forEach((b: any, i: number) => { for (const kind of ['ldg', 'external'])
            (form.elements.namedItem(`band_${i}_${kind}`) as HTMLSelectElement).value = String(b[kind]); }); }
    const retry = document.createElement('button');
    retry.dataset.i18n = 'retry';
    retry.textContent = t('retry');
    retry.type = 'button';
    retry.hidden = true;
    details.append(retry);
    const load = async () => { if (loading)
        return; loading = true; message(result, t('loading')); try {
        fill(await api('config/' + name));
        loaded = true;
        form.hidden = false;
        retry.hidden = true;
        message(result, '');
    }
    catch (e) {
        message(result, t((e as Error).message), true);
        retry.hidden = false;
    }
    finally {
        loading = false;
    } };
    retry.onclick = () => void load();
    details.ontoggle = () => { if (details.open && !loaded)
        void load(); };
    form.onsubmit = async (e) => { e.preventDefault(); const value: Record<string, unknown> = {}; for (const [key, , type] of fields) {
        const input = form.elements.namedItem(key) as HTMLInputElement;
        value[key] = type === 'checkbox' ? input.checked : type === 'number' ? Number(input.value) : input.value;
    } if (name === 'antennas')
        value.bands = bands.map((_, i) => ({ ldg: Number((form.elements.namedItem(`band_${i}_ldg`) as HTMLSelectElement).value), external: Number((form.elements.namedItem(`band_${i}_external`) as HTMLSelectElement).value) })); save.disabled = true; try {
        const saved = await api('config/' + name, { revision, value });
        revision = saved.revision;
        message(result, t(saved.restart_required ? 'savedRestart' : 'saved'));
        for (const input of form.querySelectorAll<HTMLInputElement>('input[type=password]'))
            input.value = '';
    }
    catch (e) {
        message(result, t((e as Error).message), true);
        if ((e as Error).message === 'config_conflict') {
            const comparison = document.createElement('details'), heading = document.createElement('summary');
            heading.dataset.i18n = 'loadCurrent';
            heading.textContent = t('loadCurrent');
            comparison.append(heading);
            result.after(comparison);
            comparison.ontoggle = async () => { if (!comparison.open || comparison.childElementCount > 1)
                return; try {
                const current = await api('config/' + name), pre = document.createElement('pre');
                pre.textContent = JSON.stringify(current, null, 2);
                comparison.append(pre);
                const reapply = document.createElement('button');
                reapply.type = 'button';
                reapply.dataset.i18n = 'reapply';
                reapply.textContent = t('reapply');
                reapply.onclick = () => { revision = current.revision; form.requestSubmit(); comparison.remove(); };
                comparison.append(reapply);
            }
            catch (e) {
                message(result, t((e as Error).message), true);
            } };
        }
    }
    finally {
        save.disabled = false;
    } };
    $('settings-sections').append(details);
}
$('diagnostics-button').onclick = async () => { const result = $('diagnostics').parentElement!.querySelector<HTMLElement>('.action-result')!; try {
    const values = await api('diagnostics');
    $('diagnostics').replaceChildren();
    for (const [key, value] of Object.entries(values)) {
        const dt = document.createElement('dt'), dd = document.createElement('dd');
        dt.textContent = key;
        dd.textContent = String(value);
        $('diagnostics').append(dt, dd);
    }
    message(result, '');
}
catch (e) {
    message(result, t((e as Error).message), true);
} };
for (const [id, path] of [['reconnect-mqtt', 'mqtt/reconnect'], ['restart', 'restart']])
    $(id).onclick = async () => { if (id === 'restart' && !confirm(t('restartConfirm')))
        return; const result = $(id).parentElement!.querySelector<HTMLElement>('.action-result')!; try {
        await api(path, {});
        message(result, t('accepted'));
    }
    catch (e) {
        message(result, t((e as Error).message), true);
    } };
let chartsLoaded = false, updatesLoaded = false, chartAttempt = 0, updateAttempt = 0;
async function charts() { if (chartsLoaded)
    return; try {
    const module = await import(chartAttempt++ ? './charts.js?retry=' + chartAttempt : './charts.js');
    module.start(subscribe);
    chartsLoaded = true;
    $('charts-placeholder').hidden = true;
    $('chart').hidden = false;
}
catch {
    message($('charts-placeholder').querySelector('span')!, t('network'), true);
    $('retry-charts').hidden = false;
} }
$('retry-charts').onclick = () => void charts();
let loadingBackground = false;
async function background(updatesFirst = false) { if (loadingBackground)
    return; loadingBackground = true; if (!updatesFirst)
    await charts(); try {
    if (updatesLoaded)
        return;
    const caps = await api('capabilities');
    const updates = await import(updateAttempt++ ? './updates.js?retry=' + updateAttempt : './updates.js');
    updates.start({ api, message, t, state: () => state, caps });
    updatesLoaded = true;
}
catch (e) {
    message($('release-state'), t((e as Error).message), true);
    $('check-updates').onclick = () => void background(true);
}
finally {
    loadingBackground = false;
    if (updatesFirst)
        void charts();
} }
$('check-updates').onclick = () => void background(true);
setTimeout(() => void background(), 1500);
