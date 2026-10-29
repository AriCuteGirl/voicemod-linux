// Shows launcher progress until the Voicemod frontend replaces this page.
const status = document.getElementById('status');
const log = document.getElementById('log');
const PREFIX = '[voicemod-linux] ';

window.launcher.onLine((line) => {
  // The script's own messages summarise the current step; tool output only goes to the log.
  if (line.startsWith(PREFIX)) status.textContent = line.slice(PREFIX.length);
  else if (!log.textContent) status.textContent = line;
  log.textContent += line + '\n';
  log.scrollTop = log.scrollHeight;
});

window.launcher.onFailed((message) => {
  status.textContent = message;
  status.classList.add('failed');
});
