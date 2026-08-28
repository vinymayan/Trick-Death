import { createSignal } from 'solid-js';

export type DeathMenuSettings = {
    backgroundOpacityPercent: number;
    backgroundBlurPixels: number;
    scalePercent: number;
    titleTextSizePercent: number;
    backgroundTextSizePercent: number;
    actionStyles: Record<DeathAction, DeathActionStyle>;
    labels: {
        title: string;
        backgroundText: string;
        lastSleep: string;
        checkpoint: string;
        respawn: string;
        reload: string;
        unavailableHere: string;
        unavailableLastSleep: string;
        unavailableCheckpoint: string;
        unavailableReload: string;
    };
};

export type DeathAction = 'respawn_checkpoint' | 'respawn_last_sleep' | 'respawn_here' | 'reload_save';

export type DeathActionStyle = {
    textSizePercent: number;
    buttonScalePercent: number;
};

const defaultActionStyle: DeathActionStyle = {
    textSizePercent: 100,
    buttonScalePercent: 100,
};

const defaults: DeathMenuSettings = {
    backgroundOpacityPercent: 100,
    backgroundBlurPixels: 0,
    scalePercent: 100,
    titleTextSizePercent: 100,
    backgroundTextSizePercent: 100,
    actionStyles: {
        respawn_here: { ...defaultActionStyle },
        respawn_last_sleep: { ...defaultActionStyle },
        respawn_checkpoint: { ...defaultActionStyle },
        reload_save: { ...defaultActionStyle },
    },
    labels: {
        title: 'DEFEATED',
        backgroundText: '',
        lastSleep: 'Respawn at last place slept',
        checkpoint: 'Respawn at last checkpoint',
        respawn: 'Respawn here',
        reload: 'Reload save',
        unavailableHere: 'Respawn here is blocked',
        unavailableLastSleep: 'No available last-sleep destination',
        unavailableCheckpoint: 'No available external checkpoint',
        unavailableReload: 'Reloading the current save is blocked',
    },
};

export const [settings, setSettings] = createSignal<DeathMenuSettings>(defaults);
export const [visible, setVisible] = createSignal(false);
export const [availableRespawns, setAvailableRespawns] = createSignal(0);
export const [errorMessage, setErrorMessage] = createSignal('');

declare global {
    interface Window {
        applyDeathMenuSettings: (payload: string) => void;
        showDeathMenu: (payload: string) => void;
        hideDeathMenu: (payload?: string) => void;
        showDeathMenuError: (message: string) => void;
        deathMenuAction?: (action: string) => void;
    }
}

window.applyDeathMenuSettings = (payload: string) => {
    if (!payload) return;
    let parsed: Partial<DeathMenuSettings>;
    try {
        parsed = JSON.parse(payload) as Partial<DeathMenuSettings>;
    } catch {
        return;
    }

    const current = settings();
    setSettings({
        ...current,
        ...parsed,
        actionStyles: {
            ...current.actionStyles,
            ...(parsed.actionStyles ?? {}),
            respawn_here: { ...current.actionStyles.respawn_here, ...(parsed.actionStyles?.respawn_here ?? {}) },
            respawn_last_sleep: { ...current.actionStyles.respawn_last_sleep, ...(parsed.actionStyles?.respawn_last_sleep ?? {}) },
            respawn_checkpoint: { ...current.actionStyles.respawn_checkpoint, ...(parsed.actionStyles?.respawn_checkpoint ?? {}) },
            reload_save: { ...current.actionStyles.reload_save, ...(parsed.actionStyles?.reload_save ?? {}) },
        },
        labels: { ...current.labels, ...(parsed.labels ?? {}) },
    });
};

window.showDeathMenu = (payload: string) => {
    const parsed = Number.parseInt(payload, 10);
    setAvailableRespawns(Number.isFinite(parsed) ? parsed : 0);
    setErrorMessage('');
    setVisible(true);
    window.setTimeout(() => document.querySelector<HTMLButtonElement>('.death-action:not(:disabled)')?.focus(), 0);
};

window.hideDeathMenu = () => {
    setVisible(false);
    setErrorMessage('');
};

window.showDeathMenuError = (message: string) => {
    setErrorMessage(message || 'The selected action could not be completed.');
};
