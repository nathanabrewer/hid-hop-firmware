#!/usr/bin/env node
/**
 * Key Exchange Debug - traces all events during key exchange
 */

const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

async function main() {
    // Use the two available devices
    const senderPort = '/dev/tty.usbmodem1401';
    const receiverPort = '/dev/tty.usbmodem13401';

    console.log('Using:', senderPort, 'and', receiverPort);

    const sender = new SerialPort({ path: senderPort, baudRate: 115200 });
    const receiver = new SerialPort({ path: receiverPort, baudRate: 115200 });

    const senderParser = sender.pipe(new ReadlineParser({ delimiter: '\r\n' }));
    const receiverParser = receiver.pipe(new ReadlineParser({ delimiter: '\r\n' }));

    let senderAddr = null;
    let receiverAddr = null;

    // Log ALL events from both devices
    senderParser.on('data', (line) => {
        try {
            const msg = JSON.parse(line);
            if (msg.event === 'mesh_status') {
                senderAddr = msg.addr;
            }
            console.log(`[SENDER]   ${JSON.stringify(msg)}`);
        } catch (e) {}
    });

    receiverParser.on('data', (line) => {
        try {
            const msg = JSON.parse(line);
            if (msg.event === 'mesh_status') {
                receiverAddr = msg.addr;
            }
            console.log(`[RECEIVER] ${JSON.stringify(msg)}`);
        } catch (e) {}
    });

    await new Promise(r => sender.on('open', r));
    await new Promise(r => receiver.on('open', r));
    sender.set({ dtr: true, rts: true });
    receiver.set({ dtr: true, rts: true });

    console.log('\n--- Getting device info ---');
    await new Promise(r => setTimeout(r, 1000));

    sender.write(JSON.stringify({ cmd: 'mesh_status' }) + '\n');
    receiver.write(JSON.stringify({ cmd: 'mesh_status' }) + '\n');

    await new Promise(r => setTimeout(r, 2000));

    console.log(`\nSender: ${senderAddr}, Receiver: ${receiverAddr}`);

    if (!senderAddr || !receiverAddr) {
        console.log('Failed to get addresses');
        process.exit(1);
    }

    console.log('\n--- Initiating key exchange ---');
    console.log(`Sending: {"cmd":"key_exchange","addr":"${receiverAddr}"}`);

    sender.write(JSON.stringify({ cmd: 'key_exchange', addr: receiverAddr }) + '\n');

    console.log('\n--- Waiting 5s for events ---');
    await new Promise(r => setTimeout(r, 5000));

    console.log('\n--- Checking session key status ---');
    sender.write(JSON.stringify({ cmd: 'encryption_status', addr: receiverAddr }) + '\n');

    await new Promise(r => setTimeout(r, 1000));

    console.log('\n--- Done ---');
    sender.close();
    receiver.close();
}

main().catch(err => {
    console.error('Error:', err);
    process.exit(1);
});
