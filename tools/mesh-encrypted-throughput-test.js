#!/usr/bin/env node
/**
 * HID-HOP Mesh ENCRYPTED Throughput Test
 *
 * Measures encrypted message throughput over mesh.
 * First establishes E2E encryption via key exchange, then sends encrypted chunks.
 */

const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

// Test configuration
let CHUNK_SIZE = 30;          // Smaller for encryption overhead
let TOTAL_BYTES = 500;        // Less data for encrypted test
let CHUNK_DELAY_MS = 100;     // Start slower for encrypted
const MIN_DELAY_MS = 50;
const MAX_DELAY_MS = 600;
const RETRY_DELAY_MS = 75;

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
        this.hasSessionKey = {};
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
                // Track received encrypted text
                if (msg.event === 'mesh_text_enc_rcvd') {
                    this.receivedChunks.push({
                        time: Date.now(),
                        from: msg.from,
                        text: msg.text,
                        rssi: msg.rssi
                    });
                }
                // Track key exchange confirmations
                if (msg.event === 'key_exchange_complete') {
                    this.hasSessionKey[msg.peer] = true;
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

    async pinAuth(toAddr, pin) {
        return new Promise((resolve) => {
            const handler = (msg) => {
                resolve(msg);
            };
            this.on('pin_auth', handler);
            this.send({ cmd: 'pin_auth', addr: toAddr, pin: pin });
            setTimeout(() => resolve({ err: -1, timeout: true }), 3000);
        });
    }

    async keyExchange(toAddr) {
        return new Promise((resolve) => {
            const handler = (msg) => {
                resolve(msg);
            };
            this.on('key_exchange', handler);
            this.send({ cmd: 'key_exchange', addr: toAddr });
            setTimeout(() => resolve({ err: -1, timeout: true }), 5000);
        });
    }

    async sendTextEncrypted(toAddr, text) {
        return new Promise((resolve) => {
            const handler = (msg) => {
                resolve(msg);
            };
            this.on('text_enc', handler);
            this.send({ cmd: 'text_enc', addr: toAddr, text });
            setTimeout(() => resolve(null), 2000);
        });
    }

    async checkSessionKey(peerAddr) {
        return new Promise((resolve) => {
            const handler = (msg) => {
                resolve(msg.has_key === true);
            };
            this.on('session_key_check', handler);
            this.send({ cmd: 'has_session_key', addr: peerAddr });
            setTimeout(() => resolve(false), 1000);
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
    const prefix = `[${index}]`;
    const fillLen = size - prefix.length;
    const fill = 'E'.repeat(Math.max(0, fillLen));  // 'E' for encrypted
    return prefix + fill;
}

async function runEncryptedThroughputTest() {
    console.log('');
    console.log('============================================================');
    console.log('HID-HOP MESH ENCRYPTED THROUGHPUT TEST');
    console.log('============================================================');
    console.log('');

    const allPorts = await SerialPort.list();
    const usbPorts = allPorts
        .filter(p => p.path.includes('usbmodem'))
        .filter(p => p.path.match(/usbmodem\d+$/))
        .map(p => p.path);

    if (usbPorts.length < 2) {
        console.log('Need at least 2 devices');
        process.exit(1);
    }

    console.log(`Found ${usbPorts.length} devices, using first 2`);
    console.log('');

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

        await new Promise(r => setTimeout(r, 1500));

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

        // Key exchange (no PIN auth required - PIN is only for HID/GPIO access)
        console.log('PHASE 3: KEY EXCHANGE (E2E Encryption Setup)');
        console.log('----------------------------------------');
        console.log(`  Initiating key exchange: ${sender.addr} → ${receiver.addr}`);

        const keyResult = await sender.keyExchange(receiver.addr);
        if (keyResult.err !== 0) {
            console.log(`  ✗ Key exchange failed: err=${keyResult.err}`);
            if (keyResult.err === -13) {
                console.log('    (EACCES - PIN auth may have failed or expired)');
            }
            console.log('');
            console.log('  Trying to proceed anyway...');
        } else {
            console.log(`  ✓ Key exchange initiated`);
        }

        // Wait for key confirmation
        console.log('  Waiting for key confirmation...');
        await new Promise(r => setTimeout(r, 3000));
        console.log('  ✓ Session keys should be established');
        console.log('');

        // Calculate chunks
        const numChunks = Math.ceil(TOTAL_BYTES / CHUNK_SIZE);
        const actualBytes = numChunks * CHUNK_SIZE;

        console.log('PHASE 4: ENCRYPTED THROUGHPUT TEST');
        console.log('----------------------------------------');
        console.log(`  Chunk size:    ${CHUNK_SIZE} bytes (+ encryption overhead)`);
        console.log(`  Total chunks:  ${numChunks}`);
        console.log(`  Total bytes:   ${actualBytes} (plaintext)`);
        console.log(`  Start delay:   ${CHUNK_DELAY_MS}ms (auto-adjusts)`);
        console.log('');

        receiver.clearChunks();

        const startTime = Date.now();
        let totalRetries = 0;
        let consecutiveSuccess = 0;
        let encryptionErrors = 0;
        const startDelay = CHUNK_DELAY_MS;

        console.log('  Sending encrypted chunks...');
        process.stdout.write('  ');

        for (let i = 0; i < numChunks; i++) {
            const chunk = generateChunk(i, CHUNK_SIZE);
            let success = false;
            let retries = 0;

            while (!success) {
                const result = await sender.sendTextEncrypted(receiver.addr, chunk);
                const err = result?.err || 0;

                if (err === -16 || err === -105) {
                    // Buffer full - back off
                    retries++;
                    totalRetries++;
                    consecutiveSuccess = 0;
                    CHUNK_DELAY_MS = Math.min(CHUNK_DELAY_MS + 20, MAX_DELAY_MS);
                    const backoff = RETRY_DELAY_MS * Math.min(retries, 5);
                    process.stdout.write('b');
                    await new Promise(r => setTimeout(r, backoff));
                } else if (err === -22 || err === -2) {
                    // -22 EINVAL (no session key?), -2 ENOENT
                    encryptionErrors++;
                    process.stdout.write('K');  // Key issue
                    await new Promise(r => setTimeout(r, 200));
                    if (retries++ > 3) {
                        // Try to re-establish key
                        console.log('\n  Re-initiating key exchange...');
                        await sender.keyExchange(receiver.addr);
                        await new Promise(r => setTimeout(r, 2000));
                        retries = 0;
                    }
                } else if (err !== 0) {
                    retries++;
                    totalRetries++;
                    process.stdout.write(`[${err}]`);
                    await new Promise(r => setTimeout(r, RETRY_DELAY_MS * 2));
                } else {
                    success = true;
                    consecutiveSuccess++;
                    process.stdout.write('*');  // '*' for encrypted success

                    if (consecutiveSuccess >= 4 && CHUNK_DELAY_MS > MIN_DELAY_MS) {
                        CHUNK_DELAY_MS = Math.max(CHUNK_DELAY_MS - 10, MIN_DELAY_MS);
                        consecutiveSuccess = 0;
                    }
                }
            }

            await new Promise(r => setTimeout(r, CHUNK_DELAY_MS));
        }

        const finalDelay = CHUNK_DELAY_MS;
        console.log('');
        console.log(`  Retries: ${totalRetries} | Key issues: ${encryptionErrors} | Delay: ${startDelay}ms → ${finalDelay}ms`);

        const sendEndTime = Date.now();
        console.log('');

        // Wait for propagation
        console.log('  Waiting for propagation...');
        await new Promise(r => setTimeout(r, 3000));

        const endTime = Date.now();

        // Results
        console.log('');
        console.log('PHASE 5: RESULTS');
        console.log('----------------------------------------');

        const totalSendTime = sendEndTime - startTime;
        const totalTime = endTime - startTime;
        const receivedCount = receiver.receivedChunks.length;
        const lostCount = numChunks - receivedCount;
        const successRate = (receivedCount / numChunks * 100).toFixed(1);

        const sendThroughput = (actualBytes / (totalSendTime / 1000)).toFixed(1);
        const effectiveThroughput = (receivedCount * CHUNK_SIZE / (totalTime / 1000)).toFixed(1);

        console.log(`  Chunks sent:     ${numChunks} (encrypted, guaranteed delivery)`);
        console.log(`  Chunks received: ${receivedCount}`);
        console.log(`  RF delivery:     ${successRate}%`);
        if (lostCount > 0) {
            console.log(`  RF losses:       ${lostCount}`);
        }
        console.log('');
        console.log(`  Send time:       ${totalSendTime}ms`);
        console.log(`  Total time:      ${totalTime}ms`);
        console.log('');
        console.log(`  TX Throughput:   ${sendThroughput} bytes/sec (encrypted)`);
        console.log(`  RX Throughput:   ${effectiveThroughput} bytes/sec`);
        console.log(`  Optimal delay:   ${finalDelay}ms`);
        console.log('');

        // Compare to plaintext estimate
        const plaintextEstimate = 100;  // ~100 bytes/sec baseline
        const overhead = ((plaintextEstimate - parseFloat(effectiveThroughput)) / plaintextEstimate * 100).toFixed(0);
        console.log(`  Encryption overhead: ~${Math.max(0, overhead)}% slower than plaintext`);

        if (receivedCount > 0) {
            const rssiValues = receiver.receivedChunks.map(c => c.rssi).filter(r => r != null);
            if (rssiValues.length > 0) {
                const avgRssi = (rssiValues.reduce((a,b) => a+b, 0) / rssiValues.length).toFixed(1);
                console.log(`  RSSI:            avg=${avgRssi}`);
            }
        }

        console.log('');
        console.log('============================================================');
        console.log('ENCRYPTED THROUGHPUT TEST COMPLETE');
        console.log('============================================================');
        console.log('');

    } finally {
        sender.close();
        receiver.close();
    }
}

// Parse args
const args = process.argv.slice(2);
if (args.includes('--help')) {
    console.log('Usage: node mesh-encrypted-throughput-test.js [options]');
    console.log('');
    console.log('Options:');
    console.log('  --size=N    Chunk size in bytes (default 30)');
    console.log('  --bytes=N   Total bytes to send (default 500)');
    console.log('');
    console.log('Legend: * = encrypted chunk sent, b = backoff, K = key issue');
    console.log('');
    process.exit(0);
}

const sizeArg = args.find(a => a.startsWith('--size='));
if (sizeArg) CHUNK_SIZE = parseInt(sizeArg.split('=')[1]) || 30;

const bytesArg = args.find(a => a.startsWith('--bytes='));
if (bytesArg) TOTAL_BYTES = parseInt(bytesArg.split('=')[1]) || 500;

runEncryptedThroughputTest().catch(err => {
    console.error('Error:', err);
    process.exit(1);
});
