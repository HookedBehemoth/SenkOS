const form = document.querySelector('#splash-form');
const fileInput = document.querySelector('#splash-image');
const colorInput = document.querySelector('#splash-color');
const canvas = document.querySelector('#splash-preview');
const status = document.querySelector('#splash-status');
const button = form.querySelector('button');
const context = canvas.getContext('2d', { willReadFrequently: true });
let bitmap;
let loadNumber = 0;
let converter;

function draw() {
  context.fillStyle = colorInput.value;
  context.fillRect(0, 0, canvas.width, canvas.height);
  if (bitmap) {
    const scale = Math.min(1, canvas.width / bitmap.width, canvas.height / bitmap.height);
    const width = Math.round(bitmap.width * scale);
    const height = Math.round(bitmap.height * scale);
    context.drawImage(bitmap, Math.floor((canvas.width - width) / 2), Math.floor((canvas.height - height) / 2), width, height);
  }
  canvas.hidden = !bitmap;
}

fileInput.addEventListener('change', async () => {
  const currentLoad = ++loadNumber;
  bitmap?.close();
  bitmap = undefined;
  canvas.hidden = true;
  status.textContent = '';
  const file = fileInput.files[0];
  if (!file) return;
  try {
    const next = await createImageBitmap(file);
    if (currentLoad !== loadNumber) {
      next.close();
      return;
    }
    bitmap = next;
    draw();
  } catch {
    status.textContent = 'Could not open this image.';
  }
});
colorInput.addEventListener('input', draw);

async function wasm() {
  if (!converter) {
    converter = fetch('/splash.wasm')
      .then(response => {
        if (!response.ok) throw new Error('Could not load the splash maker.');
        return response.arrayBuffer();
      })
      .then(bytes => WebAssembly.instantiate(bytes))
      .then(result => result.instance.exports);
  }
  return converter;
}

form.addEventListener('submit', async event => {
  event.preventDefault();
  if (!bitmap) {
    status.textContent = 'Choose an image first.';
    return;
  }
  button.disabled = true;
  status.textContent = 'Making splash…';
  try {
    const pixels = context.getImageData(0, 0, canvas.width, canvas.height).data;
    const core = await wasm();
    const pointer = core.allocate(pixels.length);
    if (!pointer) throw new Error('Could not allocate image memory.');
    let result;
    try {
      new Uint8Array(core.memory.buffer, pointer, pixels.length).set(pixels);
      const color = Number.parseInt(colorInput.value.slice(1), 16);
      if (core.make(pointer, pixels.length, color) !== 0) throw new Error('Could not make the splash.');
      result = new Uint8Array(core.memory.buffer, core.result_pointer(), core.result_length()).slice();
    } finally {
      core.deallocate(pointer, pixels.length);
      core.clear_result();
    }
    const url = URL.createObjectURL(new Blob([result], { type: 'application/zip' }));
    const link = document.createElement('a');
    link.href = url;
    link.download = 'senkos-splash.zip';
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 60_000);
    status.textContent = '';
  } catch (error) {
    status.textContent = error.message || 'Could not make the splash.';
  } finally {
    button.disabled = false;
  }
});
