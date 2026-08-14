import { createSignal } from 'solid-js';

export type DeathMenuSettings = {
    backgroundOpacityPercent: number;
    backgroundBlurPixels: number;
    scalePercent: number;
    labels: {
        title: string;
        checkpoint: string;
        respawn: string;
        load: string;
        noCheckpoint: string;
    };
};

const defaults: DeathMenuSettings = {
    backgroundOpacityPercent: 100,
    backgroundBlurPixels: 0,
    scalePercent: 100,
    labels: {
        title: 'DEFEATED',
        checkpoint: 'Respawn at last sleep checkpoint',
        respawn: 'Respawn here',
        load: 'Load last save',
        noCheckpoint: 'No sleep checkpoint available',
    },
};

export const [settings, setSettings] = createSignal<DeathMenuSettings>(defaults);
export const [visible, setVisible] = createSignal(false);
export const [checkpointAvailable, setCheckpointAvailable] = createSignal(false);
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
    setCheckpointAvailable(payload === '1');
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
