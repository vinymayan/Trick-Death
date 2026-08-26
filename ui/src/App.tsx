import { onMount, Show, createSignal } from 'solid-js';
import './app.css';
import { availableRespawns, errorMessage, settings, visible } from './bridge';

type DeathAction = 'respawn_checkpoint' | 'respawn_last_sleep' | 'respawn_here' | 'load_last_save';

const RESPAWN_HERE = 1;
const LAST_SLEEP = 2;
const LAST_CHECKPOINT = 4;
const LOAD_LAST_SAVE = 8;

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
                    <h1 id="death-title">{settings().labels.title}</h1>
                    <div class="death-actions">
                        <button
                            class="death-action"
                            disabled={(availableRespawns() & RESPAWN_HERE) === 0}
                            title={(availableRespawns() & RESPAWN_HERE) === 0 ? settings().labels.unavailableHere : ''}
                            onClick={() => dispatchAction('respawn_here')}
                        >
                            <span class="action-index">I</span>
                            <span>{settings().labels.respawn}</span>
                        </button>
                        <button
                            class="death-action"
                            disabled={(availableRespawns() & LAST_SLEEP) === 0}
                            title={(availableRespawns() & LAST_SLEEP) === 0 ? settings().labels.unavailableLastSleep : ''}
                            onClick={() => dispatchAction('respawn_last_sleep')}
                        >
                            <span class="action-index">II</span>
                            <span>{settings().labels.lastSleep}</span>
                        </button>
                        <button
                            class="death-action"
                            disabled={(availableRespawns() & LAST_CHECKPOINT) === 0}
                            title={(availableRespawns() & LAST_CHECKPOINT) === 0 ? settings().labels.unavailableCheckpoint : ''}
                            onClick={() => dispatchAction('respawn_checkpoint')}
                        >
                            <span class="action-index">III</span>
                            <span>{settings().labels.checkpoint}</span>
                        </button>
                        <button
                            class="death-action"
                            disabled={(availableRespawns() & LOAD_LAST_SAVE) === 0}
                            title={(availableRespawns() & LOAD_LAST_SAVE) === 0 ? settings().labels.unavailableLoad : ''}
                            onClick={() => dispatchAction('load_last_save')}
                        >
                            <span class="action-index">IV</span>
                            <span>{settings().labels.load}</span>
                        </button>
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
