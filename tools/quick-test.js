#!/usr/bin/env node
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const port1 = process.argv[2] || '/dev/tty.usbmodem13201';
const port2 = process.argv[3] || '/dev/tty.usbmodem2101';

async function testPort(path, name) {
    return new Promise((resolve) => {
        const port = new SerialPort({ path, baudRate: 115200 });
        const parser = port.pipe(new ReadlineParser({ delimiter: '\r\n' }));
        let gotResponse = false;

        parser.on('data', (line) => {
            console.log(`[${name}] ← ${line}`);
            if (line.includes('mesh_status') && !gotResponse) {
                gotResponse = true;
                setTimeout(() => {
                    port.close();
                    resolve(true);
                }, 500);
            }
        });

        port.on('open', () => {
            console.log(`[${name}] Connected to ${path}`);
            setTimeout(() => {
                console.log(`[${name}] → {"cmd":"mesh_status"}`);
                port.write('{"cmd":"mesh_status"}\n');
            }, 500);
        });

        port.on('error', (err) => {
            console.log(`[${name}] Error: ${err.message}`);
            resolve(false);
        });

        // Timeout
        setTimeout(() => {
            if (!gotResponse) {
                console.log(`[${name}] Timeout`);
                port.close();
                resolve(false);
            }
        }, 5000);
    });
}

async function main() {
    console.log('Quick mesh status test...\n');

    const r1 = await testPort(port1, 'D1');
    const r2 = await testPort(port2, 'D2');

    console.log(`\nD1: ${r1 ? 'OK' : 'FAIL'}, D2: ${r2 ? 'OK' : 'FAIL'}`);
}

main().catch(console.error);
