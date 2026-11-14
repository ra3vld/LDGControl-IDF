import { t, type State } from './main.js';
type Sample = {
    x: number;
    state: State;
};
export function start(subscribe: (callback: (s: State, at: number) => void) => void) {
    const canvas = document.getElementById('chart') as HTMLCanvasElement, context = canvas.getContext('2d')!;
    const checkbox = document.getElementById('tx-only') as HTMLInputElement;
    const styles = getComputedStyle(canvas);
    const color = (name: string) => styles.getPropertyValue(name).trim();
    const palette = { background: color('--background'), foreground: color('--foreground'), muted: color('--muted'), border: color('--border'), forward: color('--cyan'), reflected: color('--amber'), positive: color('--mint') };
    let samples: Sample[] = [], lastAt = 0, elapsed = 0, token = '', dirty = true, wasTransmitting = false;
    checkbox.onchange = () => { samples = []; lastAt = 0; elapsed = 0; dirty = true; };
    subscribe((s, at) => {
        if (s.source_token !== token) {
            samples = [];
            lastAt = 0;
            elapsed = 0;
            token = s.source_token;
        }
        const transmitting = s.connected && s.fwd_power != null && s.fwd_power > 0;
        if (checkbox.checked) {
            if (lastAt && transmitting && wasTransmitting && at - lastAt < 3000)
                elapsed += at - lastAt;
            lastAt = at;
            wasTransmitting = transmitting;
            if (!transmitting)
                return;
        }
        else
            elapsed = at;
        samples.push({ x: elapsed, state: s });
        while (samples.length && samples[0].x < elapsed - 1800000)
            samples.shift();
        dirty = true;
    });
    const resize = new ResizeObserver(() => dirty = true);
    resize.observe(canvas);
    document.addEventListener('ldg-language-change', () => { dirty = true; });
    function draw() {
        if (!dirty || !canvas.clientWidth)
            return;
        dirty = false;
        const ratio = devicePixelRatio || 1, w = canvas.clientWidth, h = 300;
        canvas.width = w * ratio;
        canvas.height = h * ratio;
        context.setTransform(ratio, 0, 0, ratio, 0, 0);
        context.fillStyle = palette.background;
        context.fillRect(0, 0, w, h);
        context.font = '12px system-ui';
        context.fillStyle = palette.muted;
        context.fillText('FWD / REF · W', 12, 18);
        context.fillText('SWR', w - 55, 18);
        const left = 50, right = w - 50, top = 30, bottom = 205, start = elapsed - 1800000;
        let maximum = 1;
        for (const p of samples)
            maximum = Math.max(maximum, p.state.fwd_power ?? 0, p.state.ref_power ?? 0);
        maximum = Math.ceil(maximum / 10) * 10;
        context.strokeStyle = palette.border;
        context.lineWidth = 1;
        for (let i = 0; i < 5; i++) {
            const y = top + (bottom - top) * i / 4;
            context.beginPath();
            context.moveTo(left, y);
            context.lineTo(right, y);
            context.stroke();
            context.fillText(String(Math.round(maximum * (1 - i / 4))), 3, y + 4);
            context.fillText((1 + 9 * (1 - i / 4)).toFixed(1), right + 8, y + 4);
        }
        // Preserve extrema per horizontal pixel instead of dropping peaks.
        for (const [key, color, scale] of [['fwd_power', palette.forward, maximum], ['ref_power', palette.reflected, maximum], ['swr', palette.foreground, 10]] as const) {
            const buckets = new Map<number, {
                min: number;
                max: number;
            }>();
            for (const sample of samples) {
                const v = sample.state[key];
                if (!sample.state.connected || v == null)
                    continue;
                const x = Math.round(left + (sample.x - start) / 1800000 * (right - left));
                const normalized = key === 'swr' ? (v - 1) / 9 : v / scale;
                const y = bottom - Math.min(1, Math.max(0, normalized)) * (bottom - top);
                const old = buckets.get(x);
                buckets.set(x, { min: Math.min(old?.min ?? y, y), max: Math.max(old?.max ?? y, y) });
            }
            context.strokeStyle = color;
            context.beginPath();
            let previous = -100;
            for (const [x, b] of buckets) {
                if (x - previous > 2)
                    context.moveTo(x, b.min);
                else
                    context.lineTo(x, b.min);
                context.lineTo(x, b.max);
                previous = x;
            }
            context.stroke();
        }
        const lanes = ['mode', 'antenna', 'band', 'radio'] as const;
        const modeColors: Record<string, string> = { '1': palette.positive, '2': palette.reflected, '3': palette.reflected, '4': palette.forward };
        for (let lane = 0; lane < 4; lane++) {
            const key = lanes[lane], y = 220 + lane * 17;
            context.fillStyle = palette.muted;
            context.fillText(['MODE', 'ANT', 'LDG', 'TCI'][lane], 3, y + 10);
            let previous = '';
            for (const sample of samples) {
                const value = key === 'radio' ? sample.state.radio?.band ?? '—' : String(sample.state[key]);
                if (value === previous)
                    continue;
                previous = value;
                const x = left + (sample.x - start) / 1800000 * (right - left);
                let hash = 0;
                for (const c of value)
                    hash = (hash * 31 + c.charCodeAt(0)) | 0;
                context.fillStyle = key === 'mode' ? modeColors[value] ?? palette.muted :
                    key === 'antenna' ? value === '1' ? palette.forward : value === '2' ? palette.positive : palette.muted :
                    [palette.forward, palette.reflected, palette.positive, palette.foreground][Math.abs(hash) % 4];
                context.fillRect(x, y, 3, 12);
                if (x < right - 25)
                    context.fillText(value, x + 4, y + 10);
            }
        }
        context.fillStyle = palette.muted;
        context.fillText(t('minutes30'), left, 296);
        context.fillText('0', right - 8, 296);
    }
    setInterval(draw, 500);
}
