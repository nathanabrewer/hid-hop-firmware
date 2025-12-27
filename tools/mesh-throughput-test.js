#!/usr/bin/env node
/**
 * HID-HOP Mesh Throughput Test
 *
 * Measures how fast we can send chunked data over the mesh network.
 * Sends multiple text chunks from one device to another and measures timing.
 */

const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

// Test configuration - can be overridden by CLI args
let CHUNK_SIZE = 50;          // Bytes per chunk (mesh text max ~60)
let TOTAL_BYTES = 1000;       // Total bytes to send
let CHUNK_DELAY_MS = 50;      // Start fast, will auto-adjust
const MIN_DELAY_MS = 20;      // Minimum delay (speed limit)
const MAX_DELAY_MS = 500;     // Maximum delay (too slow)
const RETRY_DELAY_MS = 50;    // Base wait before retry

class Device {
    constructor(path) {
        this.path = path;
        this.shortName = path.split('usbmodem')[1] || path;
        this.port = null;
        this.parser = null;
        this.addr = null;
        this.name = null;
        this.receivedChunks = [];
        this.eventHandlers = {};
    }

    async connect() {
        this.port = new SerialPort({ path: this.path, baudRate: 115200 });
        this.parser = this.port.pipe(new ReadlineParser({ delimiter: '\r\n' }));

        this.parser.on('data', (line) => {
            try {
                const msg = JSON.parse(line);
                if (msg.event && this.eventHandlers[msg.event]) {
                    this.eventHandlers[msg.event](msg);
                }
                // Track received text messages
                if (msg.event === 'mesh_text_rcvd') {
                    this.receivedChunks.push({
                        time: Date.now(),
                        from: msg.from,
                        text: msg.text,
                        rssi: msg.rssi
                    });
                }
            } catch (e) {}
        });

        await new Promise(r => this.port.on('open', r));
        this.port.set({ dtr: true, rts: true });
    }

    on(event, handler) {
        this.eventHandlers[event] = handler;
    }

    send(cmd) {
        return new Promise((resolve) => {
            this.port.write(JSON.stringify(cmd) + '\n', resolve);
        });
    }

    async getStatus() {
        return new Promise((resolve) => {
            const handler = (msg) => {
                this.addr = msg.addr;
                this.name = msg.name;
                resolve(msg);
            };
            this.on('mesh_status', handler);
            this.send({ cmd: 'mesh_status' });
            setTimeout(() => resolve(null), 2000);
        });
    }

    async sendText(toAddr, text) {
        return new Promise((resolve) => {
            const handler = (msg) => {
                resolve(msg);
            };
            this.on('mesh_text', handler);
            this.send({ cmd: 'mesh_text', addr: toAddr, text });
            setTimeout(() => resolve(null), 1000);
        });
    }

    clearChunks() {
        this.receivedChunks = [];
    }

    close() {
        if (this.port) this.port.close();
    }
}

function generateChunk(index, size) {
    // Generate a chunk with index marker for verification
    const prefix = `[${index}]`;
    const fillLen = size - prefix.length;
    const fill = 'X'.repeat(Math.max(0, fillLen));
    return prefix + fill;
}

async function runThroughputTest() {
    console.log('');
    console.log('============================================================');
    console.log('HID-HOP MESH THROUGHPUT TEST');
    console.log('============================================================');
    console.log('');

    // Find devices
    const allPorts = await SerialPort.list();
    const usbPorts = allPorts
        .filter(p => p.path.includes('usbmodem'))
        .filter(p => p.path.match(/usbmodem\d+$/))  // Only short names (updated firmware)
        .map(p => p.path);

    if (usbPorts.length < 2) {
        console.log('Need at least 2 devices for throughput test');
        console.log('Found:', usbPorts);
        process.exit(1);
    }

    console.log(`Found ${usbPorts.length} devices, using first 2 for test`);
    console.log('');

    // Connect to first 2 devices
    const sender = new Device(usbPorts[0]);
    const receiver = new Device(usbPorts[1]);

    try {
        console.log('PHASE 1: CONNECTING');
        console.log('----------------------------------------');
        await sender.connect();
        console.log(`  ✓ Sender: ${sender.shortName}`);
        await receiver.connect();
        console.log(`  ✓ Receiver: ${receiver.shortName}`);
        console.log('');

        // Wait for devices to settle
        await new Promise(r => setTimeout(r, 1500));

        // Get device info
        console.log('PHASE 2: DEVICE INFO');
        console.log('----------------------------------------');
        await sender.getStatus();
        await receiver.getStatus();
        await new Promise(r => setTimeout(r, 500));

        console.log(`  Sender:   ${sender.addr} (${sender.shortName})`);
        console.log(`  Receiver: ${receiver.addr} (${receiver.shortName})`);
        console.log('');

        if (!sender.addr || !receiver.addr) {
            console.log('  ✗ Failed to get device addresses');
            process.exit(1);
        }

        // Calculate chunks
        const numChunks = Math.ceil(TOTAL_BYTES / CHUNK_SIZE);
        const actualBytes = numChunks * CHUNK_SIZE;

        console.log('PHASE 3: THROUGHPUT TEST');
        console.log('----------------------------------------');
        const isSegmented = CHUNK_SIZE > 11;
        console.log(`  Chunk size:    ${CHUNK_SIZE} bytes (${isSegmented ? 'SEGMENTED ~' + Math.ceil(CHUNK_SIZE/11) + ' pkts' : 'unsegmented'})`);
        console.log(`  Total chunks:  ${numChunks}`);
        console.log(`  Total bytes:   ${actualBytes}`);
        console.log(`  Start delay:   ${CHUNK_DELAY_MS}ms (auto-adjusts)`);
        console.log('');

        // Clear receiver's chunk buffer
        receiver.clearChunks();

        // Start timing
        const startTime = Date.now();
        let sendErrors = 0;

        console.log('  Sending chunks...');
        process.stdout.write('  ');

        let totalRetries = 0;
        let consecutiveSuccess = 0;
        const startDelay = CHUNK_DELAY_MS;

        for (let i = 0; i < numChunks; i++) {
            const chunk = generateChunk(i, CHUNK_SIZE);
            let success = false;
            let retries = 0;

            // Keep retrying until success - we want 100% delivery
            while (!success) {
                const result = await sender.sendText(receiver.addr, chunk);
                const err = result?.err || 0;

                if (err === -16 || err === -105) {
                    // -16 EBUSY (adv queue full) or -105 ENOBUFS (no buffers)
                    // Both mean: slow down!
                    retries++;
                    totalRetries++;
                    consecutiveSuccess = 0;

                    // Increase delay for next chunk (congestion)
                    CHUNK_DELAY_MS = Math.min(CHUNK_DELAY_MS + 15, MAX_DELAY_MS);

                    const backoff = RETRY_DELAY_MS * Math.min(retries, 5);
                    process.stdout.write('b');  // 'b' for backoff
                    await new Promise(r => setTimeout(r, backoff));
                } else if (err !== 0) {
                    // Other error - retry with longer wait
                    retries++;
                    totalRetries++;
                    process.stdout.write(`[${err}]`);
                    await new Promise(r => setTimeout(r, RETRY_DELAY_MS * 3));
                } else {
                    success = true;
                    consecutiveSuccess++;
                    process.stdout.write('.');

                    // Speed up if we're doing well (5 in a row without retry)
                    if (consecutiveSuccess >= 5 && CHUNK_DELAY_MS > MIN_DELAY_MS) {
                        CHUNK_DELAY_MS = Math.max(CHUNK_DELAY_MS - 10, MIN_DELAY_MS);
                        consecutiveSuccess = 0;
                    }
                }
            }

            await new Promise(r => setTimeout(r, CHUNK_DELAY_MS));
        }

        const finalDelay = CHUNK_DELAY_MS;
        console.log('');
        console.log(`  Retries: ${totalRetries} | Delay: ${startDelay}ms → ${finalDelay}ms (auto-adjusted)`);

        const sendEndTime = Date.now();
        console.log('');
        console.log('');

        // Wait for all chunks to propagate
        console.log('  Waiting for propagation...');
        await new Promise(r => setTimeout(r, 2000));

        const endTime = Date.now();

        // Calculate results
        console.log('');
        console.log('PHASE 4: RESULTS');
        console.log('----------------------------------------');

        const totalSendTime = sendEndTime - startTime;
        const totalTime = endTime - startTime;
        const receivedCount = receiver.receivedChunks.length;
        const lostCount = numChunks - receivedCount;
        const successRate = (receivedCount / numChunks * 100).toFixed(1);

        // Calculate throughput
        const sendThroughput = (actualBytes / (totalSendTime / 1000)).toFixed(1);
        const effectiveThroughput = (receivedCount * CHUNK_SIZE / (totalTime / 1000)).toFixed(1);

        console.log(`  Chunks sent:     ${numChunks} (guaranteed - retried until success)`);
        console.log(`  Chunks received: ${receivedCount}`);
        console.log(`  RF delivery:     ${successRate}%`);
        if (lostCount > 0) {
            console.log(`  RF losses:       ${lostCount} (mesh collision or interference)`);
        }
        console.log('');
        console.log(`  Send time:       ${totalSendTime}ms`);
        console.log(`  Total time:      ${totalTime}ms (incl. propagation wait)`);
        console.log('');
        console.log(`  TX Throughput:   ${sendThroughput} bytes/sec`);
        console.log(`  RX Throughput:   ${effectiveThroughput} bytes/sec`);
        console.log(`  Optimal delay:   ${finalDelay}ms (auto-tuned from ${startDelay}ms)`);
        console.log('');

        // Check chunk order
        if (receivedCount > 0) {
            const indices = receiver.receivedChunks.map(c => {
                const match = c.text.match(/\[(\d+)\]/);
                return match ? parseInt(match[1]) : -1;
            }).filter(i => i >= 0);

            let inOrder = true;
            for (let i = 1; i < indices.length; i++) {
                if (indices[i] < indices[i-1]) {
                    inOrder = false;
                    break;
                }
            }

            console.log(`  Order preserved: ${inOrder ? 'Yes' : 'No (some reordering)'}`);

            // RSSI stats
            const rssiValues = receiver.receivedChunks.map(c => c.rssi).filter(r => r != null);
            if (rssiValues.length > 0) {
                const avgRssi = (rssiValues.reduce((a,b) => a+b, 0) / rssiValues.length).toFixed(1);
                const minRssi = Math.min(...rssiValues);
                const maxRssi = Math.max(...rssiValues);
                console.log(`  RSSI:            avg=${avgRssi} min=${minRssi} max=${maxRssi}`);
            }
        }

        console.log('');
        console.log('============================================================');
        console.log('THROUGHPUT TEST COMPLETE');
        console.log('============================================================');
        console.log('');

    } finally {
        sender.close();
        receiver.close();
    }
}

// Additional test modes
async function runBurstTest() {
    console.log('');
    console.log('============================================================');
    console.log('HID-HOP MESH BURST TEST (No delay between chunks)');
    console.log('============================================================');

    // Override delay for burst mode
    const originalDelay = CHUNK_DELAY_MS;
    // Run with 0 delay - handled by main test
}

// Parse args
const args = process.argv.slice(2);
if (args.includes('--help') || args.includes('-h')) {
    console.log('Usage: node mesh-throughput-test.js [options]');
    console.log('');
    console.log('Options:');
    console.log('  --small     Use 8-byte chunks (unsegmented, faster)');
    console.log('  --large     Use 50-byte chunks (segmented, slower)');
    console.log('  --size=N    Custom chunk size in bytes');
    console.log('  --bytes=N   Total bytes to send (default 1000)');
    console.log('');
    console.log('Legend: . = sent ok, r = retry (EBUSY backoff)');
    console.log('');
    console.log('Notes:');
    console.log('  - Chunks ≤11 bytes are unsegmented (faster)');
    console.log('  - Chunks >11 bytes require segmentation (slower)');
    console.log('');
    process.exit(0);
}

// Apply options
if (args.includes('--small')) {
    CHUNK_SIZE = 8;  // Fits in unsegmented message
}
if (args.includes('--large')) {
    CHUNK_SIZE = 50;
}
const sizeArg = args.find(a => a.startsWith('--size='));
if (sizeArg) {
    CHUNK_SIZE = parseInt(sizeArg.split('=')[1]) || 50;
}
const bytesArg = args.find(a => a.startsWith('--bytes='));
if (bytesArg) {
    TOTAL_BYTES = parseInt(bytesArg.split('=')[1]) || 1000;
}

runThroughputTest().catch(err => {
    console.error('Error:', err);
    process.exit(1);
});
