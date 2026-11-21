import type { Context } from './main.js';
type Asset = {
    name: string;
    url: string;
    browser_download_url: string;
};
type Release = {
    body?: string;
    tag_name: string;
    html_url: string;
    assets: Asset[];
};
type Manifest = {
    board: string;
    version: string;
    size: number;
    sha256: string;
    signature: string;
    url?: string;
};
export function start(ctx: Context) {
    const get = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
    const { api, message, t, caps } = ctx;
    const boardNames: Record<string, string> = { headless: 'Headless', sunton: 'Sunton', guition: 'Guition' };
    const board = String(caps.board ?? '');
    get('installed-firmware').textContent = board ? `LDGControl-IDF · ${boardNames[board] ?? board}` : 'LDGControl-IDF';
    get('installed-version').textContent = String(caps.firmware ?? '—');
    let active = false;
    const repo = String(caps.release_repository ?? '');
    async function check(manual = false) {
        if (!repo) {
            message(get('release-state'), t('current'));
            return;
        }
        const key = 'ldg.release.' + repo;
        let cache: {
            at?: number;
            etag?: string;
            release?: Release;
            retry?: number;
        } = {};
        try {
            cache = JSON.parse(localStorage.getItem(key) ?? '{}');
        }
        catch { }
        if ((cache.retry ?? 0) > Date.now()) {
            message(get('release-state'), `${t('checkFailed')} ${new Date(cache.retry!).toLocaleTimeString()}`, true);
            return;
        }
        if (!manual && cache.at && Date.now() - cache.at < 86400000) {
            if (cache.release)
                show(cache.release);
            return;
        }
        get<HTMLButtonElement>('check-updates').disabled = true;
        try {
            const response = await fetch(`https://api.github.com/repos/${repo}/releases/latest`, { headers: cache.etag ? { 'If-None-Match': cache.etag } : {}, signal: AbortSignal.timeout(12000) });
            if (response.status === 403 || response.status === 429) {
                const retry = Number(response.headers.get('retry-after'));
                const reset = Number(response.headers.get('x-ratelimit-reset'));
                cache.retry = retry ? Date.now() + retry * 1000 : reset ? reset * 1000 : Date.now() + 60000;
                throw new Error('checkFailed');
            }
            if (response.status === 404) {
                message(get('release-state'), t('current'));
                cache.at = Date.now();
                return;
            }
            if (response.status !== 304) {
                if (!response.ok)
                    throw new Error('checkFailed');
                cache.release = await response.json();
                cache.etag = response.headers.get('etag') ?? undefined;
            }
            cache.at = Date.now();
            if (cache.release)
                show(cache.release);
        }
        catch {
            message(get('release-state'), t('checkFailed'), true);
        }
        finally {
            try {
                localStorage.setItem(key, JSON.stringify(cache));
            }
            catch { }
            get<HTMLButtonElement>('check-updates').disabled = false;
        }
    }
    function newer(candidate: string, current: string) { const a = candidate.replace(/^v/, '').split('.').map(Number), b = current.replace(/^v/, '').split('.').map(Number); if (a.length !== 3 || b.length !== 3 || a.some(v => !Number.isInteger(v)) || b.some(v => !Number.isInteger(v)))
        return false; for (let i = 0; i < 3; i++) {
        if (a[i] !== b[i])
            return a[i] > b[i];
    } return false; }
    function show(release: Release) {
        const current = String(caps.firmware ?? '').replace(/^v/, '');
        message(get('release-state'), !newer(release.tag_name, current) ? t('current') : `${t('newer')}: ${release.tag_name}`);
        const list = get('release-list');
        list.replaceChildren();
        const link = document.createElement('a');
        link.href = release.html_url;
        link.target = '_blank';
        link.rel = 'noopener';
        link.textContent = release.tag_name;
        list.append(link);
        const asset = release.assets.find(a => a.name === `${caps.board}.manifest.json`);
        if (!asset)
            return;
        const install = document.createElement('button');
        install.type = 'button';
        install.dataset.i18n = 'installRelease';
        install.textContent = t('installRelease');
        install.onclick = async () => {
            install.disabled = true;
            try {
                // The REST asset endpoint advertises CORS. If binary redirects deny
                // CORS, the visible file-upload path remains available.
                let manifest: Manifest;
                const embedded = release.body?.match(/<!-- ldg-manifests\n([\s\S]*?)\n-->/);
                if (embedded) {
                    manifest = JSON.parse(embedded[1])[String(caps.board)];
                }
                else {
                    const response = await fetch(asset.url, { headers: { Accept: 'application/octet-stream' }, signal: AbortSignal.timeout(10000) });
                    if (!response.ok)
                        throw new Error('checkFailed');
                    manifest = await response.json() as Manifest;
                }
                const binary = release.assets.find(a => a.name === `${caps.board}.bin`);
                if (!binary)
                    throw new Error('checkFailed');
                await prepare(manifest);
                await longRequest('update/download', { url: binary.browser_download_url });
                await reconnect();
            }
            catch (e) {
                message(get('release-state'), t((e as Error).message), true);
                active = false;
            }
            finally {
                install.disabled = false;
            }
        };
        list.append(install);
    }
    async function longRequest(path: string, body: unknown) { const response = await fetch('/api/v2/' + path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body), signal: AbortSignal.timeout(path.endsWith('download') ? 600000 : 45000) }); const data = await response.json(); if (!response.ok)
        throw new Error(data.error ?? 'failed'); return data; }
    async function prepare(manifest: Manifest) { if (manifest.board !== caps.board)
        throw new Error('invalid_request'); active = true; get<HTMLProgressElement>('ota-progress').hidden = false; get('cancel-update').hidden = false; message(get('ota-state'), t('accepted')); try {
        await longRequest('update/prepare', manifest);
        for (let i = 0; i < 95; i++) {
            const state = await api('update');
            if (state.error)
                throw new Error(state.error);
            if (state.stage === 'receiving')
                return;
            if (!active)
                throw new Error('cancelled');
            await new Promise(r => setTimeout(r, 1000));
        }
        throw new Error('update_idle_timeout');
    }
    catch (e) {
        active = false;
        throw e;
    } }
    async function reconnect() { message(get('ota-state'), t('accepted')); for (let i = 0; i < 40; i++) {
        await new Promise(r => setTimeout(r, 1000));
        try {
            await api('capabilities');
            if (i > 2) {
                location.reload();
                return;
            }
        }
        catch { }
    } throw new Error('network'); }
    get<HTMLFormElement>('upload-form').onsubmit = async (e) => {
        e.preventDefault();
        const form = e.currentTarget as HTMLFormElement, result = form.querySelector<HTMLElement>('.action-result')!, button = form.querySelector<HTMLButtonElement>('button')!;
        button.disabled = true;
        try {
            const file = (form.elements.namedItem('firmware') as HTMLInputElement).files![0], meta = (form.elements.namedItem('manifest') as HTMLInputElement).files![0];
            if (meta.size > 4096)
                throw new Error('invalid_request');
            const manifest = JSON.parse(await meta.text()) as Manifest;
            if (file.size !== manifest.size)
                throw new Error('invalid_request');
            await prepare(manifest);
            const response = await fetch('/api/v2/update/upload', { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: file });
            if (!response.ok) {
                const failure = await response.json();
                throw new Error(failure.error ?? 'failed');
            }
            await reconnect();
        }
        catch (e) {
            message(result, t((e as Error).message), true);
            active = false;
        }
        finally {
            button.disabled = false;
        }
    };
    get('cancel-update').onclick = async () => { try {
        await api('update/cancel', {});
        active = false;
        message(get('ota-state'), t('cancelled'));
    }
    catch (e) {
        message(get('ota-state'), t((e as Error).message), true);
    } };
    setInterval(async () => { if (!active)
        return; try {
        const state = await api('update');
        const progress = get<HTMLProgressElement>('ota-progress');
        progress.value = state.total ? state.bytes / state.total * 100 : 0;
        message(get('ota-state'), state.error ? t(state.error) : `${t(state.stage)} · ${Math.floor(progress.value)}%`, !!state.error);
        if (state.error || state.stage === 'ready') {
            active = false;
            get('cancel-update').hidden = true;
        }
    }
    catch { } }, 1000);
    get('check-updates').onclick = () => void check(true);
    void check();
}
