import type { State } from './main.js';
export type Sample = { x: number; wall: number; segment: number; state: State };
export class History {
    samples: Sample[] = [];
    elapsed = 0;
    private at: number | null = null;
    private transmitting = false;
    private source = '';
    private segment = 0;
    constructor(public txOnly = false) {}
    reset(txOnly = this.txOnly) {
        this.txOnly = txOnly; this.samples = []; this.elapsed = 0;
        this.at = null; this.transmitting = false; this.segment = 0;
    }
    add(state: State, wall: number) {
        const source = state.boot_id + ':' + state.source_token;
        if (source !== this.source || (this.at !== null && wall < this.at)) {
            this.reset(); this.source = source;
        }
        const radio = state.radio;
        const ready = radio?.provider === 'tci' && radio.state === 'ready' &&
            typeof radio.age_ms === 'number' && radio.age_ms >= 0 && radio.age_ms <= 3000;
        const tx = ready && (radio.rx_tx === 'tx' || radio.tuning === true) ? true :
            ready && radio.rx_tx === 'rx' && radio.tuning === false ? false :
            state.connected && state.meter_age_ms <= 3000 && Number.isFinite(state.fwd_power) && (state.fwd_power ?? 0) > 0;
        const continuous = this.at !== null && wall - this.at <= 5000;
        if (this.txOnly && continuous && this.transmitting) this.elapsed += wall - this.at!;
        if (!continuous || (this.txOnly && !this.transmitting)) this.segment++;
        const previousTx = this.transmitting;
        this.at = wall; this.transmitting = tx;
        if (!this.txOnly) this.elapsed = wall;
        if (!this.txOnly || tx) this.samples.push({ x: this.elapsed, wall, segment: this.segment, state });
        else if (continuous && previousTx && this.samples.length) {
            const last = this.samples[this.samples.length - 1];
            this.samples.push({ ...last, x: this.elapsed, wall });
        }
        while (this.samples.length && this.samples[0].x < this.elapsed - 1800000) this.samples.shift();
    }
}
