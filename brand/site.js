'use strict';

(function () {
  const root = document.documentElement;
  const themeToggle = document.getElementById('theme-toggle');
  const themeLabel = document.getElementById('theme-label');
  const toast = document.getElementById('status-toast');
  let toastTimeout;
  const themes = {
    dark: {canvas:'#111317',surface:'#1A1D22',raised:'#24282F',text:'#F5F4F0',muted:'#B5B7BE',accent:'#FF877D',primary:'#FF5B50',border:'#78808A'},
    light: {canvas:'#FAF9F6',surface:'#FFFFFF',raised:'#F0EFEB',text:'#24252B',muted:'#5D626A',accent:'#B92323',primary:'#C52524',border:'#747A83'}
  };

  function announce(message) {
    window.clearTimeout(toastTimeout);
    toast.textContent = message;
    toast.classList.add('is-visible');
    toastTimeout = window.setTimeout(function () { toast.classList.remove('is-visible'); }, 2600);
  }

  function applyTheme(theme) {
    root.dataset.theme = theme;
    const isDark = theme === 'dark';
    themeToggle.setAttribute('aria-label', 'Switch to ' + (isDark ? 'light' : 'dark') + ' theme');
    themeToggle.setAttribute('aria-pressed', String(!isDark));
    themeLabel.textContent = isDark ? 'Light' : 'Dark';
    document.querySelector('meta[name="theme-color"]').setAttribute('content', themes[theme].canvas);
    document.querySelectorAll('[data-themed-logo]').forEach(function (img) {
      img.src = 'assets/logos/conflux-' + img.dataset.themedLogo + '-' + theme + '.svg';
    });
    document.querySelectorAll('[data-token]').forEach(function (button) {
      const hex = themes[theme][button.dataset.token];
      button.dataset.copy = hex;
      button.querySelector('i').style.backgroundColor = hex;
      button.querySelector('small').textContent = hex;
      button.setAttribute('aria-label', 'Copy ' + button.querySelector('strong').textContent + ' ' + hex);
    });
    try { localStorage.setItem('conflux-theme', theme); } catch (_) { /* The board also works without browser storage. */ }
  }
  themeToggle.addEventListener('click', function () { applyTheme(root.dataset.theme === 'dark' ? 'light' : 'dark'); });
  applyTheme(root.dataset.theme === 'light' ? 'light' : 'dark');

  async function copyText(text) {
    try {
      if (navigator.clipboard && window.isSecureContext) {
        await navigator.clipboard.writeText(text);
      } else {
        const input = document.createElement('textarea');
        input.value = text;
        input.setAttribute('readonly', '');
        input.style.position = 'fixed';
        input.style.opacity = '0';
        document.body.appendChild(input);
        const focused = document.activeElement;
        input.select();
        const copied = document.execCommand('copy');
        input.remove();
        if (focused && focused.focus) focused.focus({preventScroll:true});
        if (!copied) throw new Error('Clipboard unavailable');
      }
      announce(text + ' copied to clipboard');
    } catch (_) {
      announce('Select and copy this value: ' + text);
    }
  }
  document.querySelectorAll('[data-copy], [data-token]').forEach(function (button) {
    button.addEventListener('click', function () { copyText(button.dataset.copy); });
  });

  function updateLogo() {
    const layout = document.querySelector('input[name="logo-layout"]:checked').value;
    const background = document.querySelector('input[name="logo-background"]:checked').value;
    const filename = 'conflux-' + layout + '-' + background + '.svg';
    const stage = document.getElementById('logo-stage');
    const preview = document.getElementById('logo-preview');
    stage.dataset.layout = layout;
    stage.dataset.background = background;
    preview.src = 'assets/logos/' + filename;
    preview.alt = 'Conflux ' + layout + ' logo on a ' + background + ' background';
    document.getElementById('logo-download').href = preview.getAttribute('src');
    document.getElementById('logo-filename').textContent = filename;
    document.getElementById('logo-specimen-label').textContent = layout.toUpperCase() + ' / ' + background.toUpperCase() + ' BACKGROUND';
  }
  document.querySelectorAll('input[name="logo-layout"],input[name="logo-background"]').forEach(function (input) { input.addEventListener('change', updateLogo); });
  updateLogo();

  if (window.location.protocol === 'file:') document.getElementById('zip-download').href = '../CONFLUX-brand-pack.zip';

  const ns = 'http://www.w3.org/2000/svg';
  document.querySelectorAll('[data-waveform]').forEach(function (wave) {
    const offset = wave.dataset.waveform === 'a' ? 2 : 7;
    const group = wave.querySelector('.wave-bars');
    for (let i = 0; i < 140; i++) {
      const height = 7 + Math.abs(Math.sin(i * .73 + offset) * Math.cos(i * .17)) * 65 + Math.abs(Math.sin(i * .23 + offset)) * 11;
      const bar = document.createElementNS(ns, 'rect');
      bar.setAttribute('x', String(i * 5));
      bar.setAttribute('y', String((88 - height) / 2));
      bar.setAttribute('width', '3.2');
      bar.setAttribute('height', String(height));
      group.appendChild(bar);
    }
  });

  document.querySelectorAll('.knob-control').forEach(function (label) {
    const input = label.querySelector('input');
    function updateKnob() {
      const value = Number(input.value);
      label.querySelector('.knob').style.setProperty('--rotation', (value * 10) + 'deg');
      label.querySelector('output').textContent = (value > 0 ? '+' : value < 0 ? '−' : '') + Math.abs(value) + ' dB';
      input.setAttribute('aria-valuetext', value + ' decibels, visual concept only');
    }
    input.addEventListener('input', updateKnob);
    updateKnob();
  });

  const wet = document.getElementById('fx-wet');
  function updateWet() {
    document.getElementById('fx-output').textContent = wet.value + '%';
    wet.setAttribute('aria-valuetext', wet.value + ' percent, visual concept only');
  }
  wet.addEventListener('input', updateWet);
  document.querySelectorAll('[data-effect]').forEach(function (button) {
    button.addEventListener('click', function () {
      document.querySelectorAll('[data-effect]').forEach(function (other) {
        const active = other === button;
        other.classList.toggle('is-active', active);
        other.setAttribute('aria-pressed', String(active));
      });
      document.getElementById('fx-type').textContent = button.dataset.effect.toUpperCase();
    });
  });
  updateWet();

  const previewButton = document.getElementById('transition-preview');
  const directorState = document.getElementById('director-state');
  const consoleNote = document.getElementById('console-note');
  const progress = document.getElementById('plan-progress');
  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
  let frame = 0;
  let finishTimer = 0;

  function resetTransport() {
    window.cancelAnimationFrame(frame);
    window.clearTimeout(finishTimer);
    progress.style.width = '0%';
    directorState.textContent = 'Transition ready';
    document.getElementById('deck-a-state').textContent = '● PLAYING';
    document.getElementById('deck-b-state').textContent = '◇ ARMED';
    document.getElementById('deck-a-time').textContent = '02:34 / 06:12';
    document.getElementById('deck-b-time').textContent = '00:32 / 05:46';
    previewButton.disabled = false;
    previewButton.innerHTML = 'Preview transition <span aria-hidden="true">▶</span>';
    consoleNote.textContent = 'Visual preview only. Try the EQ and effect controls.';
    document.querySelectorAll('.waveform .playhead').forEach(function (line) { line.style.left = '34%'; });
  }

  function finishTransition() {
    progress.style.width = '100%';
    directorState.textContent = 'B is in. Your mix continues.';
    document.getElementById('deck-a-state').textContent = '◇ COMPLETE';
    document.getElementById('deck-b-state').textContent = '● PLAYING';
    previewButton.disabled = false;
    previewButton.innerHTML = 'Replay visual preview <span aria-hidden="true">↺</span>';
    consoleNote.textContent = 'Transition visualized. Your EQ and effect settings stayed yours.';
    announce('Visual transition complete. EQ and effect settings were preserved.');
  }

  previewButton.addEventListener('click', function () {
    resetTransport();
    previewButton.disabled = true;
    previewButton.textContent = 'Visualizing…';
    directorState.textContent = 'Blending A → B';
    document.getElementById('deck-b-state').textContent = '● MIXING IN';
    consoleNote.textContent = 'A condensed visual demonstration. No audio is playing.';
    if (reducedMotion.matches) {
      progress.style.width = '100%';
      finishTimer = window.setTimeout(finishTransition, 500);
      return;
    }
    const started = performance.now();
    const duration = 6000;
    function tick(now) {
      const position = Math.min(1, (now - started) / duration);
      progress.style.width = (position * 100).toFixed(1) + '%';
      document.querySelectorAll('.waveform .playhead').forEach(function (line) { line.style.left = (34 + position * 37).toFixed(1) + '%'; });
      const seconds = Math.floor(position * 6);
      document.getElementById('deck-a-time').textContent = '02:' + String(34 + seconds).padStart(2, '0') + ' / 06:12';
      document.getElementById('deck-b-time').textContent = '00:' + String(32 + seconds).padStart(2, '0') + ' / 05:46';
      if (position < 1) frame = window.requestAnimationFrame(tick);
      else finishTransition();
    }
    frame = window.requestAnimationFrame(tick);
  });

  document.getElementById('reset-concept').addEventListener('click', function () {
    resetTransport();
    document.querySelectorAll('.knob-control input').forEach(function (input) {
      input.value = input.defaultValue;
      input.dispatchEvent(new Event('input'));
    });
    wet.value = wet.defaultValue;
    updateWet();
    document.querySelector('[data-effect="Filter"]').click();
    announce('Application appearance preview reset');
  });

  const motionButton = document.getElementById('motion-demo');
  let motionTimer;
  motionButton.addEventListener('click', function () {
    window.clearTimeout(motionTimer);
    motionButton.classList.remove('is-playing');
    if (reducedMotion.matches) { announce('Reduced motion is enabled. Example stays still.'); return; }
    window.requestAnimationFrame(function () {
      window.requestAnimationFrame(function () {
        motionButton.classList.add('is-playing');
        motionTimer = window.setTimeout(function () { motionButton.classList.remove('is-playing'); }, 900);
      });
    });
  });
})();
