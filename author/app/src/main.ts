import { mount } from 'svelte';
import App from './app.svelte';
import './lib/ui/theme.css';

mount(App, { target: document.getElementById('app')! });
