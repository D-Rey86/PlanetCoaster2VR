// PC2VR extension. Uses the game's existing Gameface components, focus and input.
// No additional view, camera, render target or controller polling loop.
import * as preact from '/js/common/lib/preact.js';
import { OptionsMenuBasePanel } from '/js/project/modules/options/components/OptionsMenuBasePanel.js';
import { OptionsMenuBasePanelSection } from '/js/project/modules/options/components/OptionsMenuBasePanelSection.js';
import { OptionsMenuListItemSlider, OptionsMenuListItemOnOffStepper } from '/js/project/modules/options/components/OptionsMenuListItem.js';
import { Button } from '/js/project/components/Button.js';
import { stringLiteral } from '/js/common/util/LocalisationUtil.js';

let sequence = 0;
function rpc(path) {
    return new Promise((resolve, reject) => {
        const request = new XMLHttpRequest();
        request.open('GET', '/pc2vr/' + path + (path.includes('?') ? '&' : '?') + 'r=' + (++sequence), true);
        request.timeout = 5000;
        request.onload = () => {
            try {
                const result = JSON.parse(request.responseText);
                if (!result.ok) throw new Error(result.error || 'VR setting was not applied');
                resolve(result);
            } catch (error) { reject(error); }
        };
        request.onerror = request.ontimeout = () => reject(new Error('VR settings connection unavailable'));
        request.send();
    });
}
const defaults = {
    Resolution: 100, ExpandedCoverage: 0, Foveated: 1, UIWidth: 55, UIDistance: 150, UIHeight: -5, UIHorizontal: 0, UIFollowHmd: 0,
    CheekyEnabled: 0, CheekyWidth: 55, CheekyHeight: 45, CheekyPeripheralDlaa: 1,
    CheekyPeripheralScale: 75, CheekyCenterSupersampling: 100,
    CheekyCenterMode: 0, CheekyRoundness: 0, CheekyTransition: 4,
    CheekyAlignedHeight: 0, CheekyManualHeight: 0, CheekyBorder: 0,
    CheekyGazeSmoothing: 20, CheekyGazeHold: 400
};
const cheekyNames = {
    CheekyEnabled: ['Enabled', false], CheekyWidth: ['Width', true], CheekyHeight: ['Height', true],
    CheekyPeripheralDlaa: ['PeripheralDlaa', false], CheekyPeripheralScale: ['PeripheralDlaaScale', true],
    CheekyCenterSupersampling: ['CenterSupersampling', true],
    CheekyCenterMode: ['CenterMode', false], CheekyRoundness: ['Roundness', true],
    CheekyTransition: ['TransitionWidth', true], CheekyAlignedHeight: ['AlignedHeightOffset', true],
    CheekyManualHeight: ['HeightOffset', true], CheekyBorder: ['AlignmentBorder', false],
    CheekyGazeSmoothing: ['GazeSmoothingMs', false], CheekyGazeHold: ['GazeHoldMs', false]
};
// CheekyGazeSnapshotV1 status bits. Read only on the existing visible-menu poll.
const gazeFlags = { supported: 4, focused: 8, valid: 32, mapping: 64, unsupportedLayout: 128 };
export function trackingStatus(cheeky) {
    if (!cheeky?.ready) return 'Eye tracking: Cheeky is unavailable.';
    const settings = cheeky.settings;
    if (!settings || settings.CenterMode === undefined) return 'Eye tracking: Waiting for status.';
    if (settings.CenterMode === 0) return 'Eye tracking: Off (fixed center selected).';
    if (settings.CenterMode === 2) return 'Eye tracking: Simulated gaze selected; headset tracking is not in use.';
    if (settings.CenterMode !== 1) return 'Eye tracking: Unsupported center mode.';
    if (!settings.Enabled && !(settings.NrEnabled && settings.NrFoveated))
        return 'Eye tracking: Selected, but foveated DLSS is off.';
    const gaze = cheeky.gaze;
    if (!gaze) return 'Eye tracking: Waiting for status.';
    if (!gaze.layer || !gaze.abi) return 'Eye tracking: Runtime adapter unavailable. Using fixed fallback.';
    const flags = Number(gaze.status_flags) || 0;
    if (!(flags & gazeFlags.supported)) return 'Eye tracking: Not provided by the runtime. Using fixed fallback.';
    if (flags & gazeFlags.unsupportedLayout) return 'Eye tracking: Stereo layout unsupported. Using fixed fallback.';
    if (!(flags & gazeFlags.focused)) return 'Eye tracking: VR session not focused. Using fixed fallback.';
    if (!(flags & gazeFlags.valid)) return 'Eye tracking: No valid signal. Holding briefly or using fixed fallback.';
    if (gaze.ambiguous || !(flags & gazeFlags.mapping) || !gaze.using_gaze)
        return 'Eye tracking: Waiting for a fresh sample or reliable eye mapping.';
    return 'Eye tracking: Active; the high-quality region follows your eyes.';
}
export class VRSettingsPanel extends preact.Component {
    state = { values: { ...defaults }, loaded: false, available: false, message: '', renderSize: '',
        tracking: 'Eye tracking: Waiting for status.', mapping: 'Eye mapping: Waiting for status.',
        autoAlignment: true, recalibrating: false };
    // The shipping Preact owns _dirty and other internal fields. Keep mod state
    // distinct so rendering cannot overwrite queued settings edits.
    _vrDirty = {};
    _vrPending = {};
    _vrSerial = Promise.resolve();
    _vrPoll = null;
    _vrDebounce = null;
    _vrUnmounted = false;
    _vrReading = false;
    _vrRecalibrating = false;
    componentDidMount() {
        this.refresh();
        this.updatePolling();
    }
    componentDidUpdate(previousProps) {
        if (previousProps.hidden != this.props.hidden) this.updatePolling();
    }
    componentWillUnmount() {
        this._vrUnmounted = true;
        clearInterval(this._vrPoll);
        clearTimeout(this._vrDebounce);
        this.flush();
    }
    updatePolling() {
        clearInterval(this._vrPoll); this._vrPoll = null;
        if (!this.props.hidden) {
            this.refresh();
            this._vrPoll = setInterval(() => this.refresh(), 1000);
        }
    }
    refresh() {
        if (this._vrReading || this._vrUnmounted) return;
        this._vrReading = true;
        rpc('state.json').then(data => {
            if (this._vrUnmounted) return;
            const values = { ...defaults };
            for (const name of Object.keys(defaults)) {
                if (data[name] !== undefined) values[name] = data[name];
            }
            const settings = data.cheeky?.settings;
            if (settings) {
                for (const name of Object.keys(cheekyNames)) {
                    const [nativeName, fractional] = cheekyNames[name];
                    if (settings[nativeName] !== undefined)
                        values[name] = fractional ? Math.round(settings[nativeName] * 100) : Number(settings[nativeName]);
                }
            }
            Object.assign(values, this._vrDirty);
            const available = !!(data.nvidia && data.cheeky?.ready);
            const renderSize = data.renderWidth ? data.renderWidth + ' x ' + data.renderHeight : 'Waiting for VR';
            const tracking = trackingStatus(data.nvidia ? data.cheeky : null);
            const mapping = 'Eye mapping: ' + (available ? data.cheeky.eye_calibration?.status || 'Waiting for calibration.' : 'Cheeky is unavailable.');
            const autoAlignment = settings?.AutoStereoAlignment !== false;
            if (this.state.loaded && this.state.available === available && this.state.renderSize === renderSize &&
                this.state.tracking === tracking && this.state.mapping === mapping && this.state.autoAlignment === autoAlignment &&
                Object.keys(defaults).every(name => this.state.values[name] === values[name])) return;
            this.setState({ values, loaded: true, available, renderSize, tracking, mapping, autoAlignment });
        }).catch(error => {
            if (!this._vrUnmounted) this.setState({ message: error.message,
                tracking: 'Eye tracking: Status unavailable (connection lost).', mapping: 'Eye mapping: Status unavailable.' });
        }).finally(() => { this._vrReading = false; });
    }
    change(name, value, immediate = false) {
        value = Math.round(Number(value));
        if (!Number.isFinite(value) || this.state.values[name] === value) return;
        this._vrDirty[name] = value;
        this._vrPending[name] = value;
        this.setState({ values: { ...this.state.values, [name]: value }, message: '' });
        clearTimeout(this._vrDebounce);
        if (immediate) this.flush();
        else this._vrDebounce = setTimeout(() => this.flush(), 150);
    }
    flush() {
        const pending = this._vrPending; this._vrPending = {};
        for (const name of Object.keys(pending)) {
            const value = pending[name];
            this._vrSerial = this._vrSerial.then(() => rpc('set.json?key=' + name + '&value=' + value)).then(() => {
                if (this._vrDirty[name] === value) delete this._vrDirty[name];
            }).catch(error => {
                if (this._vrDirty[name] === value) delete this._vrDirty[name];
                if (!this._vrUnmounted) this.setState({ message: error.message });
            });
        }
    }
    recalibrate() {
        if (this._vrRecalibrating || !this.state.loaded || !this.state.available) return;
        this._vrRecalibrating = true;
        clearTimeout(this._vrDebounce);
        this.flush(); // Geometry edits must reach Cheeky before the explicit action.
        this.setState({ recalibrating: true, message: '' });
        this._vrSerial = this._vrSerial.then(() => rpc('recalibrate.json')).then(() => {
            if (!this._vrUnmounted) this.setState({ message: 'Recalibration requested. Markers may appear briefly.' });
        }).catch(error => {
            if (!this._vrUnmounted) this.setState({ message: error.message });
        }).finally(() => {
            this._vrRecalibrating = false;
            if (!this._vrUnmounted) { this.setState({ recalibrating: false }); this.refresh(); }
        });
    }
    slider(name, label, min, max, step, unit, description, cheeky = false) {
        return preact.h(OptionsMenuListItemSlider, {
            key: name, label: stringLiteral(label), min, max, step, value: this.state.values[name],
            formatter: value => stringLiteral(Math.round(value) + unit),
            disabled: !this.state.loaded || (cheeky && !this.state.available),
            optionsMenuInfoData: { infoHeader: stringLiteral(label), infoDescription: stringLiteral(description) },
            onChange: value => this.change(name, value)
        });
    }
    toggle(name, label, description, cheeky = false) {
        return preact.h(OptionsMenuListItemOnOffStepper, {
            key: name, label: stringLiteral(label), toggled: name === 'CheekyCenterMode' ? this.state.values[name] === 1 : !!this.state.values[name],
            disabled: !this.state.loaded || (cheeky && !this.state.available),
            optionsMenuInfoData: { infoHeader: stringLiteral(label), infoDescription: stringLiteral(description) },
            onChange: value => this.change(name, value ? 1 : 0, true)
        });
    }
    render(props, state) {
        const section = (header, children) => preact.h(OptionsMenuBasePanelSection, { header: stringLiteral(header), panelIndex: props.panelIndex }, children);
        return preact.h(OptionsMenuBasePanel, {
            hidden: props.hidden, focusable: !props.hidden, header: stringLiteral('Virtual Reality'),
            infoHeader: stringLiteral('VR settings'), infoDescription: stringLiteral('Changes save automatically. DLSS users: model E is recommended through your game-specific driver profile.')
        },
        section('Resolution (' + state.renderSize + ')', [
            this.toggle('ExpandedCoverage', 'Expanded headset coverage', 'Off uses the original FOV and rendering behavior. On improves edge coverage for wide or angled displays and head tilt, but can increase rendering cost. Applies during play.'),
            this.slider('Resolution', 'VR render resolution', 25, 200, 5, '%',
                'Scales the headset-recommended pixel density while preserving its field of view. Lower values improve performance; higher values improve detail.'),
            this.toggle('Foveated', 'Foveated rendering', 'Keeps full shading detail near each eye\'s center and reduces shading toward the edges on supported GPUs. Turning it off restores full shading throughout the scene. Separate from Cheeky foveated DLSS.')
        ]),
        section('UI placement', [
            this.toggle('UIFollowHmd', 'Follow headset', 'Off keeps the current recentered placement. On moves the HUD and its selection circle with your headset. Fixed menus keep their picture and UI together.'),
            this.slider('UIWidth', 'UI width', 20, 120, 1, ' deg', 'Angular width of the HUD and pause-menu panel.'),
            this.slider('UIDistance', 'UI distance', 30, 1000, 10, ' cm', 'Distance of the HUD and pause-menu panel. This changes its stereo depth.'),
            this.slider('UIHorizontal', 'UI horizontal position', -60, 60, 1, ' deg', 'Moves the HUD to the left or right. Positive values move it right.'),
            this.slider('UIHeight', 'UI vertical position', -60, 60, 1, ' deg', 'Moves the HUD above or below eye level. Positive values move it up.'),
            preact.h(Button, { label: stringLiteral('Recenter VR'), icon: '/img/icons/restart.svg', modifiers: 'bigger',
                onSelect: () => rpc('recenter.json').catch(error => this.setState({ message: error.message })) })
        ]),
        section('Cheeky foveated DLSS', [
            preact.h('div', { style: { padding: '0.5rem', flexShrink: 0, fontSize: '0.85rem', minHeight: '2.5rem' } }, state.available ?
                'Requires DLSS in Graphics settings. Use the driver profile to select model E.' :
                'Requires a compatible NVIDIA RTX GPU and the optional Cheeky DLSS package.'),
            this.toggle('CheekyEnabled', 'Enable foveated DLSS', 'Reconstructs a higher-quality center and a cheaper outer area. Turning it off uses ordinary DLSS.', true),
            this.slider('CheekyWidth', 'High-quality center width', 20, 100, 1, '%', 'A wider center preserves more detail but increases GPU work.', true),
            this.slider('CheekyHeight', 'High-quality center height', 20, 100, 1, '%', 'A taller center preserves more detail but increases GPU work.', true),
            this.slider('CheekyRoundness', 'Region roundness', 0, 100, 5, '%', 'Rounds the visible region blend. This does not reduce the rectangular DLSS rendering workload.', true),
            this.slider('CheekyTransition', 'Region transition width', 0, 30, 1, '%', 'Blends the high-quality center into the periphery. Higher values soften the boundary.', true),
            this.slider(state.autoAlignment ? 'CheekyAlignedHeight' : 'CheekyManualHeight', 'Region vertical position', -100, 100, 1, '%',
                'Moves the region vertically. Negative values move it up; positive values move it down. Also biases eye-tracked placement.', true),
            this.toggle('CheekyBorder', 'Show high-quality region border', 'Shows a red border to check region placement and eye tracking. Separate from calibration markers.', true),
            this.toggle('CheekyPeripheralDlaa', 'Higher-quality periphery', 'Applies DLAA outside the center. Turning it off improves performance and reduces peripheral detail.', true),
            this.slider('CheekyPeripheralScale', 'Peripheral DLAA resolution', 20, 100, 5, '%', 'Resolution used for peripheral DLAA when higher-quality periphery is enabled.', true),
            this.slider('CheekyCenterSupersampling', 'Center supersampling', 100, 200, 5, '%', 'Values above 100% increase central reconstruction cost.', true)
        ]),
        section('Cheeky eye tracking', [
            this.toggle('CheekyCenterMode', 'Use eye tracking', 'Off uses a fixed center. On follows valid runtime gaze, with a fixed fallback when tracking is unavailable. Requires DLSS and a runtime that provides eye tracking. Controls Cheeky only.', true),
            preact.h('div', { style: { padding: '0.5rem', flexShrink: 0, fontSize: '0.85rem' } }, state.tracking),
            preact.h('div', { style: { padding: '0.5rem', flexShrink: 0, fontSize: '0.85rem' } }, state.mapping),
            this.slider('CheekyGazeSmoothing', 'Eye-tracking smoothing', 0, 100, 5, ' ms', 'Higher values smooth gaze movement but delay the high-quality region following your eyes.', true),
            this.slider('CheekyGazeHold', 'Hold on tracking loss', 0, 1000, 50, ' ms', 'Keeps the last gaze briefly during blinks or signal loss, then returns to fixed placement.', true),
            preact.h(Button, { label: stringLiteral('Recalibrate Cheeky'), icon: '/img/icons/restart.svg', modifiers: 'bigger',
                disabled: !state.loaded || !state.available || state.recalibrating,
                optionsMenuInfoData: { infoHeader: stringLiteral('Recalibrate Cheeky'),
                    infoDescription: stringLiteral('Rechecks eye-image alignment. Calibration markers may appear briefly. Does not reset your image-quality settings.') },
                onSelect: () => this.recalibrate() })
        ]),
        state.message && preact.h('div', { style: { padding: '0.5rem', flexShrink: 0 } }, state.message));
    }
}
export function installVRMenu(OptionsMenuBase, renderer, Tab) {
    if (!OptionsMenuBase?.prototype || OptionsMenuBase.prototype.__pc2vrInstalled) return;
    const original = OptionsMenuBase.prototype.componentWillMount;
    if (typeof original !== 'function') return;
    OptionsMenuBase.prototype.componentWillMount = function (...args) {
        original.apply(this, args);
        if (!Array.isArray(this._panelData) || !Array.isArray(this._tabs) || !Array.isArray(this._panels)) return;
        const panelIndex = this._panels.length;
        this._panelData.push({ tooltip: stringLiteral('Virtual Reality'), component: VRSettingsPanel });
        this._tabs.push(renderer.h(Tab, { label: stringLiteral('VR'), tooltip: stringLiteral('Virtual Reality'), modifiers: 'alt narrow' }));
        this._panels.push(renderer.h(VRSettingsPanel, { panelIndex, mode: this.props.mode }));
    };
    OptionsMenuBase.prototype.__pc2vrInstalled = true;
}
