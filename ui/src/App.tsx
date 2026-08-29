import { onMount, Show, createSignal } from 'solid-js';
import './app.css';
import { availableRespawns, errorMessage, settings, visible } from './bridge';
import type { DeathAction } from './bridge';

const RESPAWN_HERE = 1;
const LAST_SLEEP = 2;
const LAST_CHECKPOINT = 4;
const RELOAD_SAVE = 8;

const supportedBackgroundExtensions = ['svg', 'png', 'webp', 'jpg', 'jpeg', 'gif', 'avif'];

function findBackground(): Promise<string> {
    const candidates = supportedBackgroundExtensions.map((extension) => `./assets/Background.${extension}`);
    return new Promise((resolve) => {
        let index = 0;
        const tryNext = () => {
            if (index >= candidates.length) {
                resolve('');
                return;
            }
            const candidate = candidates[index++];
            const image = new Image();
            image.onload = () => resolve(candidate);
            image.onerror = tryNext;
            image.src = candidate;
        };
        tryNext();
    });
}

function dispatchAction(action: DeathAction) {
    if (typeof window.deathMenuAction === 'function') {
        window.deathMenuAction(action);
    }
}

function actionStyle(action: DeathAction) {
    const style = settings().actionStyles[action];
    const buttonScale = style.buttonScalePercent / 100;
    const textScale = style.textSizePercent / 100;
    return [
        `--button-min-height:${62 * buttonScale}px`,
        `--button-padding-y:${8 * buttonScale}px`,
        `--button-padding-x:${18 * buttonScale}px`,
        `--button-side-column:${54 * buttonScale}px`,
        `--button-width:${Math.min(150, style.buttonScalePercent)}%`,
        `--button-font-size:${21 * textScale}px`,
        `--button-index-size:${16 * textScale}px`,
    ].join(';');
}

function actionLabel(action: DeathAction, label: string) {
    const cost = settings().resourceCosts[action];
    return cost.configured ? `${label} (${cost.owned}x)` : label;
}

function actionDisabled(action: DeathAction) {
    const cost = settings().resourceCosts[action];
    return cost.configured && !cost.affordable;
}

function App() {
    const [background, setBackground] = createSignal('');
    onMount(() => void findBackground().then(setBackground));

    return (
        <Show when={visible()}>
            <main
                class="death-screen"
                aria-modal="true"
                role="dialog"
                aria-labelledby="death-title"
            >
                <Show when={background()}>
                    <img
                        class="death-background"
                        src={background()}
                        alt=""
                        style={{
                            opacity: String(settings().backgroundOpacityPercent / 100),
                            filter: `blur(${settings().backgroundBlurPixels}px)`,
                        }}
                    />
                </Show>
                <section
                    class="death-panel"
                    style={{ transform: `scale(${settings().scalePercent / 100})` }}
                >
                    <div class="death-rule" />
                    <h1
                        id="death-title"
                        style={`font-size:clamp(${44 * settings().titleTextSizePercent / 100}px,${7 * settings().titleTextSizePercent / 100}vw,${92 * settings().titleTextSizePercent / 100}px)`}
                    >
                        {settings().labels.title}
                    </h1>
                    <Show when={settings().labels.backgroundText}>
                        <p
                            class="death-message"
                            style={`font-size:${18 * settings().backgroundTextSizePercent / 100}px`}
                        >
                            {settings().labels.backgroundText}
                        </p>
                    </Show>
                    <div class="death-actions">
                        <Show when={(availableRespawns() & RESPAWN_HERE) !== 0}>
                            <button
                                class="death-action"
                                style={actionStyle('respawn_here')}
                                disabled={actionDisabled('respawn_here')}
                                onClick={() => dispatchAction('respawn_here')}
                            >
                                <span class="action-index">I</span>
                                <span>{actionLabel('respawn_here', settings().labels.respawn)}</span>
                            </button>
                        </Show>
                        <Show when={(availableRespawns() & LAST_SLEEP) !== 0}>
                            <button
                                class="death-action"
                                style={actionStyle('respawn_last_sleep')}
                                disabled={actionDisabled('respawn_last_sleep')}
                                onClick={() => dispatchAction('respawn_last_sleep')}
                            >
                                <span class="action-index">II</span>
                                <span>{actionLabel('respawn_last_sleep', settings().labels.lastSleep)}</span>
                            </button>
                        </Show>
                        <Show when={(availableRespawns() & LAST_CHECKPOINT) !== 0}>
                            <button
                                class="death-action"
                                style={actionStyle('respawn_checkpoint')}
                                disabled={actionDisabled('respawn_checkpoint')}
                                onClick={() => dispatchAction('respawn_checkpoint')}
                            >
                                <span class="action-index">III</span>
                                <span>{actionLabel('respawn_checkpoint', settings().labels.checkpoint)}</span>
                            </button>
                        </Show>
                        <Show when={(availableRespawns() & RELOAD_SAVE) !== 0}>
                            <button
                                class="death-action"
                                style={actionStyle('reload_save')}
                                onClick={() => dispatchAction('reload_save')}
                            >
                                <span class="action-index">IV</span>
                                <span>{settings().labels.reload}</span>
                            </button>
                        </Show>
                    </div>
                    <Show when={errorMessage()}>
                        <p class="death-error" role="alert">{errorMessage()}</p>
                    </Show>
                    <div class="death-rule bottom" />
                </section>
            </main>
        </Show>
    );
}

export default App;
