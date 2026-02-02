/**
 * MQTT Connection Test Script
 * ============================
 * Use this script to test your MQTT connection before running the full server.
 * 
 * Usage: node test-mqtt.js
 */

const mqtt = require('mqtt');
const dotenv = require('dotenv');

// Load environment variables
dotenv.config({ path: './config.env' });

const brokerUrl = process.env.MQTT_BROKER_URL || 'mqtts://broker.hivemq.com:8883';
const username = process.env.MQTT_USERNAME || '';
const password = process.env.MQTT_PASSWORD || '';

console.log('============================================');
console.log('   MQTT Connection Test');
console.log('============================================');
console.log(`Broker: ${brokerUrl}`);
console.log(`Username: ${username || '(none)'}`);
console.log('');
console.log('Connecting...');

const options = {
  username: username,
  password: password,
  clientId: `test_client_${Math.random().toString(16).substr(2, 8)}`,
  clean: true,
  connectTimeout: 30000,
  reconnectPeriod: 0  // Don't auto-reconnect for this test
};

const client = mqtt.connect(brokerUrl, options);

client.on('connect', () => {
  console.log('✅ Successfully connected to MQTT broker!');
  console.log('');
  
  // Subscribe to test topic
  client.subscribe('energy/#', (err) => {
    if (err) {
      console.error('❌ Subscribe error:', err);
    } else {
      console.log('📡 Subscribed to energy/# topics');
    }
  });
  
  // Publish a test message
  const testMessage = JSON.stringify({
    test: true,
    timestamp: new Date().toISOString(),
    message: 'Hello from test script!'
  });
  
  client.publish('energy/test', testMessage, (err) => {
    if (err) {
      console.error('❌ Publish error:', err);
    } else {
      console.log('📤 Published test message to energy/test');
    }
  });
  
  console.log('');
  console.log('Waiting for messages (press Ctrl+C to exit)...');
  console.log('If ESP32 is running, you should see data below:');
  console.log('--------------------------------------------');
});

client.on('message', (topic, payload) => {
  const message = payload.toString();
  console.log(`📨 [${topic}]: ${message.substring(0, 100)}${message.length > 100 ? '...' : ''}`);
});

client.on('error', (error) => {
  console.error('❌ Connection error:', error.message);
  console.log('');
  console.log('Troubleshooting tips:');
  console.log('1. Check your MQTT broker URL format');
  console.log('2. Verify username and password');
  console.log('3. Ensure port 8883 is not blocked');
  console.log('4. Check if HiveMQ cluster is running');
  process.exit(1);
});

client.on('close', () => {
  console.log('Connection closed');
});

// Handle Ctrl+C
process.on('SIGINT', () => {
  console.log('');
  console.log('Disconnecting...');
  client.end();
  process.exit(0);
});
