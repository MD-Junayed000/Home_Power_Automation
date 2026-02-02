/**
 * ESP32 Energy Meter - Backend Server
 * ====================================
 * 
 * This server:
 * 1. Connects to HiveMQ Cloud MQTT broker
 * 2. Subscribes to energy meter data topics
 * 3. Stores data in MongoDB
 * 4. Provides REST API for dashboard
 * 5. WebSocket for real-time updates
 */

const express = require('express');
const cors = require('cors');
const mongoose = require('mongoose');
const mqtt = require('mqtt');
const WebSocket = require('ws');
const path = require('path');
const dotenv = require('dotenv');

// Load environment variables
dotenv.config({ path: './config.env' });

// ============== Configuration ==============
const config = {
  mqtt: {
    brokerUrl: process.env.MQTT_BROKER_URL || 'mqtts://broker.hivemq.com:8883',
    username: process.env.MQTT_USERNAME || '',
    password: process.env.MQTT_PASSWORD || '',
    topics: {
      data: 'energy/data',
      status: 'energy/status',
      control: 'energy/control',
      relay: 'energy/relay'
    }
  },
  mongodb: {
    uri: process.env.MONGO_URI || 'mongodb://localhost:27017/energydb'
  },
  server: {
    port: process.env.PORT || 3000
  }
};

// ============== Express App Setup ==============
const app = express();
app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, '../static')));

// ============== MongoDB Schemas ==============
const ReadingSchema = new mongoose.Schema({
  timestamp: { type: Date, default: Date.now },
  voltage: Number,
  current: Number,
  power: Number,
  energy: Number,
  balance: Number,
  relayState: Boolean,
  faultDetected: Boolean,
  faultReason: String,
  rssi: Number
});

const StatusLogSchema = new mongoose.Schema({
  timestamp: { type: Date, default: Date.now },
  status: String,
  faultReason: String,
  relayState: Boolean,
  voltage: Number,
  current: Number,
  balance: Number
});

const SettingsSchema = new mongoose.Schema({
  overVoltageThreshold: { type: Number, default: 260.0 },
  underVoltageThreshold: { type: Number, default: 160.0 },
  overCurrentThreshold: { type: Number, default: 10.0 },
  costPerKWh: { type: Number, default: 8.0 },
  minimumBalance: { type: Number, default: 0 },
  updatedAt: { type: Date, default: Date.now }
});

// Create Models
const Reading = mongoose.model('Reading', ReadingSchema);
const StatusLog = mongoose.model('StatusLog', StatusLogSchema);
const Settings = mongoose.model('Settings', SettingsSchema);

// ============== Global State ==============
let latestReading = {
  voltage: 0,
  current: 0,
  power: 0,
  energy: 0,
  balance: 500,
  relayState: true,
  faultDetected: false,
  faultReason: '',
  manualOverride: false,
  overVoltageThreshold: 260,
  underVoltageThreshold: 160,
  overCurrentThreshold: 10,
  costPerKWh: 8,
  rssi: 0,
  uptime: 0,
  lastUpdate: null,
  isOnline: false
};

let mqttClient = null;
let wsClients = [];

// ============== MongoDB Connection ==============
async function connectMongoDB() {
  try {
    await mongoose.connect(config.mongodb.uri, {
      useNewUrlParser: true,
      useUnifiedTopology: true
    });
    console.log('✅ Connected to MongoDB');
    
    // Load or create default settings
    let settings = await Settings.findOne();
    if (!settings) {
      settings = await Settings.create({});
      console.log('📝 Created default settings');
    }
    
    // Update latestReading with saved settings
    latestReading.overVoltageThreshold = settings.overVoltageThreshold;
    latestReading.underVoltageThreshold = settings.underVoltageThreshold;
    latestReading.overCurrentThreshold = settings.overCurrentThreshold;
    latestReading.costPerKWh = settings.costPerKWh;
    
  } catch (error) {
    console.error('❌ MongoDB connection error:', error.message);
    console.log('⚠️  Running without database - data will not be persisted');
  }
}

// ============== MQTT Connection ==============
function connectMQTT() {
  console.log('🔌 Connecting to MQTT broker...');
  console.log(`   URL: ${config.mqtt.brokerUrl}`);
  
  const options = {
    username: config.mqtt.username,
    password: config.mqtt.password,
    clientId: `backend_server_${Math.random().toString(16).substr(2, 8)}`,
    clean: true,
    connectTimeout: 30000,
    reconnectPeriod: 5000,
    rejectUnauthorized: true
  };
  
  mqttClient = mqtt.connect(config.mqtt.brokerUrl, options);
  
  mqttClient.on('connect', () => {
    console.log('✅ Connected to MQTT broker');
    
    // Subscribe to topics
    Object.values(config.mqtt.topics).forEach(topic => {
      if (topic !== config.mqtt.topics.control) {
        mqttClient.subscribe(topic, (err) => {
          if (err) {
            console.error(`❌ Error subscribing to ${topic}:`, err);
          } else {
            console.log(`📡 Subscribed to: ${topic}`);
          }
        });
      }
    });
  });
  
  mqttClient.on('message', async (topic, payload) => {
    try {
      const message = payload.toString();
      const data = JSON.parse(message);
      
      console.log(`📨 Message on [${topic}]:`, JSON.stringify(data).substring(0, 100));
      
      // Handle different topics
      if (topic === config.mqtt.topics.data) {
        handleDataMessage(data);
      } else if (topic === config.mqtt.topics.status) {
        handleStatusMessage(data);
      } else if (topic === config.mqtt.topics.relay) {
        handleRelayMessage(data);
      }
      
      // Broadcast to WebSocket clients
      broadcastToClients({ topic, data });
      
    } catch (error) {
      console.error('Error processing MQTT message:', error);
    }
  });
  
  mqttClient.on('error', (error) => {
    console.error('❌ MQTT error:', error.message);
  });
  
  mqttClient.on('close', () => {
    console.log('🔌 MQTT connection closed');
    latestReading.isOnline = false;
    broadcastToClients({ topic: 'status', data: { isOnline: false } });
  });
  
  mqttClient.on('reconnect', () => {
    console.log('🔄 MQTT reconnecting...');
  });
}

// ============== Message Handlers ==============
async function handleDataMessage(data) {
  // Update latest reading
  latestReading = {
    ...latestReading,
    ...data,
    lastUpdate: new Date(),
    isOnline: true
  };
  
  // Save to MongoDB (with rate limiting)
  try {
    if (mongoose.connection.readyState === 1) {
      await Reading.create({
        voltage: data.voltage,
        current: data.current,
        power: data.power,
        energy: data.energy,
        balance: data.balance,
        relayState: data.relayState,
        faultDetected: data.faultDetected,
        faultReason: data.faultReason,
        rssi: data.rssi
      });
    }
  } catch (error) {
    console.error('Error saving reading:', error.message);
  }
}

async function handleStatusMessage(data) {
  latestReading.isOnline = data.status === 'online' || data.status === 'ok';
  
  // Log status changes
  try {
    if (mongoose.connection.readyState === 1) {
      await StatusLog.create({
        status: data.status,
        faultReason: data.faultReason,
        relayState: data.relayState,
        voltage: data.voltage,
        current: data.current,
        balance: data.balance
      });
    }
  } catch (error) {
    console.error('Error saving status:', error.message);
  }
}

function handleRelayMessage(data) {
  latestReading.relayState = data.state;
  latestReading.manualOverride = data.manual;
}

// ============== WebSocket Server ==============
function setupWebSocket(server) {
  const wss = new WebSocket.Server({ server });
  
  wss.on('connection', (ws) => {
    console.log('🔗 WebSocket client connected');
    wsClients.push(ws);
    
    // Send current state
    ws.send(JSON.stringify({ topic: 'init', data: latestReading }));
    
    ws.on('close', () => {
      console.log('🔌 WebSocket client disconnected');
      wsClients = wsClients.filter(client => client !== ws);
    });
    
    ws.on('error', (error) => {
      console.error('WebSocket error:', error);
    });
  });
}

function broadcastToClients(message) {
  const messageStr = JSON.stringify(message);
  wsClients.forEach(client => {
    if (client.readyState === WebSocket.OPEN) {
      client.send(messageStr);
    }
  });
}

// ============== REST API Routes ==============

// Get latest data
app.get('/api/data', (req, res) => {
  res.json({
    isValid: latestReading.isOnline,
    voltage: latestReading.voltage,
    current: latestReading.current,
    power: latestReading.power / 1000, // Convert to kW for dashboard compatibility
    energy: latestReading.energy,
    balance: latestReading.balance,
    relayState: latestReading.relayState,
    faultDetected: latestReading.faultDetected,
    theftDetected: false,
    overVoltageThreshold: latestReading.overVoltageThreshold,
    overCurrentThreshold: latestReading.overCurrentThreshold,
    theftCurrentThreshold: 0.02,
    minimumBalance: 0,
    costPerKWh: latestReading.costPerKWh,
    manualOverride: latestReading.manualOverride
  });
});

// Control relay
app.post('/api/setRelay', (req, res) => {
  const state = req.query.state || req.body.state;
  const override = req.query.override || req.body.override;
  
  if (state === undefined) {
    return res.status(400).json({ success: false, message: 'Missing state parameter' });
  }
  
  const relayState = state === '1' || state === 1 || state === true;
  
  // Publish to MQTT
  if (mqttClient && mqttClient.connected) {
    mqttClient.publish(config.mqtt.topics.control, JSON.stringify({
      relay: relayState
    }));
    
    console.log(`📤 Sent relay command: ${relayState ? 'ON' : 'OFF'}`);
    res.json({ success: true });
  } else {
    res.status(503).json({ success: false, message: 'MQTT not connected' });
  }
});

// Recharge balance
app.post('/api/recharge', (req, res) => {
  const amount = parseFloat(req.query.amount || req.body.amount);
  
  if (!amount || amount <= 0) {
    return res.status(400).json({ success: false, message: 'Invalid amount' });
  }
  
  // Publish to MQTT
  if (mqttClient && mqttClient.connected) {
    mqttClient.publish(config.mqtt.topics.control, JSON.stringify({
      recharge: amount
    }));
    
    // Update local state immediately for UI feedback
    latestReading.balance += amount;
    
    console.log(`📤 Sent recharge command: ${amount} Taka`);
    res.json({ success: true, balance: latestReading.balance });
  } else {
    res.status(503).json({ success: false, message: 'MQTT not connected' });
  }
});

// Update thresholds
app.post('/api/setThresholds', async (req, res) => {
  try {
    const { overVoltage, overCurrent, theftCurrent, minBalance, costPerKWh } = req.body;
    
    // Publish to MQTT
    if (mqttClient && mqttClient.connected) {
      mqttClient.publish(config.mqtt.topics.control, JSON.stringify({
        overVoltage: parseFloat(overVoltage),
        overCurrent: parseFloat(overCurrent),
        costPerKWh: parseFloat(costPerKWh),
        save: true
      }));
      
      // Update local state
      latestReading.overVoltageThreshold = parseFloat(overVoltage);
      latestReading.overCurrentThreshold = parseFloat(overCurrent);
      latestReading.costPerKWh = parseFloat(costPerKWh);
      
      // Save to MongoDB
      if (mongoose.connection.readyState === 1) {
        await Settings.findOneAndUpdate({}, {
          overVoltageThreshold: parseFloat(overVoltage),
          overCurrentThreshold: parseFloat(overCurrent),
          costPerKWh: parseFloat(costPerKWh),
          updatedAt: new Date()
        }, { upsert: true });
      }
      
      console.log(`📤 Updated thresholds`);
      res.json({ success: true });
    } else {
      res.status(503).json({ success: false, message: 'MQTT not connected' });
    }
  } catch (error) {
    res.status(500).json({ success: false, message: error.message });
  }
});

// Reset energy
app.post('/api/reset', (req, res) => {
  if (mqttClient && mqttClient.connected) {
    mqttClient.publish(config.mqtt.topics.control, JSON.stringify({
      resetEnergy: true
    }));
    
    latestReading.energy = 0;
    
    console.log(`📤 Sent energy reset command`);
    res.json({ success: true });
  } else {
    res.status(503).json({ success: false, message: 'MQTT not connected' });
  }
});

// Factory reset
app.post('/api/factoryReset', (req, res) => {
  if (mqttClient && mqttClient.connected) {
    mqttClient.publish(config.mqtt.topics.control, JSON.stringify({
      factoryReset: true
    }));
    
    console.log(`📤 Sent factory reset command`);
    res.json({ success: true });
  } else {
    res.status(503).json({ success: false, message: 'MQTT not connected' });
  }
});

// Get historical data
app.get('/api/history', async (req, res) => {
  try {
    const hours = parseInt(req.query.hours) || 24;
    const since = new Date(Date.now() - hours * 60 * 60 * 1000);
    
    if (mongoose.connection.readyState !== 1) {
      return res.json([]);
    }
    
    const readings = await Reading.find({
      timestamp: { $gte: since }
    }).sort({ timestamp: 1 }).limit(1000);
    
    res.json(readings);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

// Get aggregated report data for charts
app.get('/api/reports', async (req, res) => {
  try {
    const range = req.query.range || 'day';
    
    if (mongoose.connection.readyState !== 1) {
      return res.json({ 
        success: false, 
        error: 'Database not connected',
        data: [] 
      });
    }
    
    let hours, groupBy, minutes;
    switch (range) {
      case '10min':
        minutes = 10;
        hours = 10 / 60;
        groupBy = 'minute';
        break;
      case '1hour':
        hours = 1;
        groupBy = 'minute';
        break;
      case '6hour':
        hours = 6;
        groupBy = 'hour';
        break;
      case 'week':
        hours = 7 * 24;
        groupBy = 'day';
        break;
      case 'month':
        hours = 30 * 24;
        groupBy = 'day';
        break;
      default: // day
        hours = 24;
        groupBy = 'hour';
    }
    
    const since = new Date(Date.now() - hours * 60 * 60 * 1000);
    
    // Get all readings in the time range
    const readings = await Reading.find({
      timestamp: { $gte: since }
    }).sort({ timestamp: 1 });
    
    if (readings.length === 0) {
      return res.json({
        success: true,
        data: [],
        summary: {
          totalEnergy: 0,
          avgPower: 0,
          peakPower: 0,
          peakTime: null,
          totalReadings: 0
        }
      });
    }
    
    // Aggregate data by minute, hour, or day
    const aggregated = {};
    let totalEnergy = 0;
    let peakPower = 0;
    let peakTime = null;
    let totalPower = 0;
    
    readings.forEach(reading => {
      const date = new Date(reading.timestamp);
      let key;
      
      if (groupBy === 'minute') {
        // Group by minute for fine-grained view
        key = `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')} ${String(date.getHours()).padStart(2, '0')}:${String(date.getMinutes()).padStart(2, '0')}`;
      } else if (groupBy === 'hour') {
        key = `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')} ${String(date.getHours()).padStart(2, '0')}:00`;
      } else {
        key = `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`;
      }
      
      if (!aggregated[key]) {
        aggregated[key] = {
          timestamp: key,
          energy: 0,
          power: 0,
          voltage: 0,
          current: 0,
          count: 0,
          maxPower: 0
        };
      }
      
      aggregated[key].energy = reading.energy || 0; // Use latest energy value
      aggregated[key].power += reading.power || 0;
      aggregated[key].voltage += reading.voltage || 0;
      aggregated[key].current += reading.current || 0;
      aggregated[key].count++;
      
      if ((reading.power || 0) > aggregated[key].maxPower) {
        aggregated[key].maxPower = reading.power || 0;
      }
      
      if ((reading.power || 0) > peakPower) {
        peakPower = reading.power || 0;
        peakTime = reading.timestamp;
      }
      
      totalPower += reading.power || 0;
    });
    
    // Convert to array and calculate averages
    const dataArray = Object.values(aggregated).map(item => ({
      timestamp: item.timestamp,
      energy: item.energy,
      avgPower: item.count > 0 ? item.power / item.count : 0,
      maxPower: item.maxPower,
      avgVoltage: item.count > 0 ? item.voltage / item.count : 0,
      avgCurrent: item.count > 0 ? item.current / item.count : 0,
      readings: item.count
    }));
    
    // Sort by timestamp
    dataArray.sort((a, b) => new Date(a.timestamp) - new Date(b.timestamp));
    
    // Calculate total energy from the difference between first and last reading
    const firstReading = readings[0];
    const lastReading = readings[readings.length - 1];
    totalEnergy = (lastReading.energy || 0) - (firstReading.energy || 0);
    if (totalEnergy < 0) totalEnergy = lastReading.energy || 0;
    
    res.json({
      success: true,
      range: range,
      data: dataArray,
      summary: {
        totalEnergy: totalEnergy,
        avgPower: readings.length > 0 ? totalPower / readings.length : 0,
        peakPower: peakPower,
        peakTime: peakTime,
        totalReadings: readings.length,
        firstReading: firstReading ? firstReading.timestamp : null,
        lastReading: lastReading ? lastReading.timestamp : null
      }
    });
  } catch (error) {
    console.error('Reports API error:', error);
    res.status(500).json({ success: false, error: error.message });
  }
});

// Get system status
app.get('/api/status', (req, res) => {
  res.json({
    isOnline: latestReading.isOnline,
    lastUpdate: latestReading.lastUpdate,
    mqttConnected: mqttClient ? mqttClient.connected : false,
    mongoConnected: mongoose.connection.readyState === 1
  });
});

// Serve main dashboard
app.get('/', (req, res) => {
  res.sendFile(path.join(__dirname, '../static/index.html'));
});

// Backward compatibility routes
app.get('/data', (req, res) => res.redirect('/api/data'));
app.post('/recharge', (req, res) => {
  req.query = { ...req.query, ...req.body };
  res.redirect(307, '/api/recharge');
});
app.post('/setRelay', (req, res) => {
  req.query = { ...req.query, ...req.body };
  res.redirect(307, '/api/setRelay');
});
app.post('/setThresholds', (req, res) => res.redirect(307, '/api/setThresholds'));
app.post('/reset', (req, res) => res.redirect(307, '/api/reset'));
app.post('/factoryReset', (req, res) => res.redirect(307, '/api/factoryReset'));

// ============== Start Server ==============
async function startServer() {
  // Connect to MongoDB
  await connectMongoDB();
  
  // Connect to MQTT
  connectMQTT();
  
  // Start HTTP server
  const server = app.listen(config.server.port, () => {
    console.log('');
    console.log('============================================');
    console.log('   ESP32 Energy Meter - Backend Server');
    console.log('============================================');
    console.log(`🚀 Server running on http://localhost:${config.server.port}`);
    console.log(`📊 Dashboard: http://localhost:${config.server.port}`);
    console.log(`🔌 API: http://localhost:${config.server.port}/api/data`);
    console.log('============================================');
    console.log('');
  });
  
  // Setup WebSocket
  setupWebSocket(server);
}

// Handle graceful shutdown
process.on('SIGINT', async () => {
  console.log('\n🛑 Shutting down...');
  
  if (mqttClient) {
    mqttClient.end();
  }
  
  if (mongoose.connection.readyState === 1) {
    await mongoose.connection.close();
  }
  
  process.exit(0);
});

// Start the server
startServer();
