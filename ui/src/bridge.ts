import { createSignal } from 'solid-js';

export type DeathMenuSettings = {
    backgroundOpacityPercent: number;
    backgroundBlurPixels: number;
    scalePercent: number;
    labels: {
        title: string;
        lastSleep: string;
        checkpoint: string;
        respawn: string;
        load: string;
        unavailableHere: string;
        unavailableLastSleep: string;
        unavailableCheckpoint: string;
        unavailableLoad: string;
    };
};

const defaults: DeathMenuSettings = {
    backgroundOpacityPercent: 100,
    backgroundBlurPixels: 0,
    scalePercent: 100,
    labels: {
        title: 'DEFEATED',
        lastSleep: 'Respawn at last place slept',
        checkpoint: 'Respawn at last checkpoint',
        respawn: 'Respawn here',
        load: 'Load last save',
        unavailableHere: 'Respawn here is blocked',
        unavailableLastSleep: 'No available last-sleep destination',
        unavailableCheckpoint: 'No available external checkpoint',
        unavailableLoad: 'Loading the last save is blocked',
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
