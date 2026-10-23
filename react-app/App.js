import React, { useState, useEffect, useRef } from 'react';
import { StyleSheet, Text, View, ScrollView, TouchableOpacity, TextInput } from 'react-native';
import { StatusBar } from 'expo-status-bar'
import 'react-native-url-polyfill/auto'
import { ref, onValue } from 'firebase/database';
import { ref as dbRef, set } from 'firebase/database';
import { db } from './firebase';
//import mqtt from 'mqtt';

const BROKER_URL      = 'wss://broker.emqx.io:8083/mqtt'
const TELEMETRY_TOPIC = 'ucf/security/nodes/+/telemetry'
const CMD_TOPIC       = 'ucf/security/commands/all'
// ============================================================
// CONFIG — swap these in when you're ready to connect
// ============================================================
const MQTT_CONFIG = {
  host: 'broker.emqx.io',
  port: 8084,                  // WSS port for HiveMQ
  path: '/mqtt',               // required for HiveMQ WSS
};

const SUBSCRIBE_TOPIC = 'ucf/security/#';
const STALE_MS        = 6000;
const NODES = ['node-0', 'node-1' ]; // placeholder node IDs

// ============================================================
// APP
// ============================================================
export default function App() {
  const [connected, setConnected] = useState(false);
  const [messages, setMessages] = useState([]);
  const [nodes, setNodes]         = useState({})
  const [armed, setArmed] = useState(false);
  const [, setTick]       = useState(0);
const [intervalInputs, setIntervalInputs] = useState({});
 // const clientRef = useRef(null);

 useEffect(() => {
  const nodesRef = ref(db, 'nodes');

  const unsubscribe = onValue(nodesRef, (snapshot) => {
    const data = snapshot.val();

    if (!data) {
      setNodes({});
      return;
    }

    const formatted = {};

    Object.keys(data).forEach((nodeId) => {
      formatted[nodeId] = {
        node: nodeId,
        ...data[nodeId],
        lastSeen: Date.now(),
      }; });

    setNodes(formatted);
    setConnected(true);
  });

  return () => unsubscribe();
}, []);
  // -----------------------------------------
  // Publish helper
  // -----------------------------------------
const sendCommand = (nodeId, command) => {
  set(dbRef(db, `commands/${nodeId}`), command);
  console.log('Command sent:', nodeId, command);
};

const handleArmToggle = () => {
  const next = !armed;
  setArmed(next);

  NODES.forEach((n) => {
    sendCommand(n, next ? 'arm_on' : 'arm_off');
  });
};

const handleBlink = (nodeId) => {
  sendCommand(nodeId, 'blink');
};
const handleSetInterval = (nodeId) => {
  const value = intervalInputs[nodeId];
  if (!value) return;

  sendCommand(nodeId, {
    type: 'set_interval',
    value: Number(value),
  });
  console.log('Set interval:', nodeId, value);
};

  // -----------------------------------------
  // UI
  // -----------------------------------------
  const nodeList = Object.values(nodes).sort((a, b) => a.node.localeCompare(b.node));

  return (
    <View style={styles.container}>
      <StatusBar style="light" />

      {/* Header */}
      <View style={styles.header}>
        <View>
          <Text style={styles.title}>Dashboard</Text>
          <Text style={styles.subtitle}>
            {nodeList.length} {nodeList.length === 1 ? 'node' : 'nodes'} online
          </Text>
        </View>
        <View style={[styles.pill, { backgroundColor: connected ? 'rgba(74,222,128,0.15)' : 'rgba(113,113,122,0.15)' }]}>
          <View style={[styles.pillDot, { backgroundColor: connected ? '#4ade80' : '#71717a' }]} />
          <Text style={[styles.pillText, { color: connected ? '#4ade80' : '#71717a' }]}>
            {connected ? 'LIVE' : 'OFFLINE'}
          </Text>
        </View>
      </View>

      {/* Arm / disarm */}
      <TouchableOpacity
        style={[styles.armButton, { backgroundColor: armed ? '#ef4444' : '#4ade80' }]}
        onPress={handleArmToggle}>
        <Text style={styles.armButtonText}>
          {armed ? 'ON — tap to toggle' : 'OFF — tap to toggle'}
        </Text>
      </TouchableOpacity>

<View style={styles.nodeRow}>
  {NODES.map((n) => (
    <TouchableOpacity key={n} style={styles.nodeButton} onPress={() => handleBlink(n)}>
      <Text style={styles.nodeButtonText}>Blink {n}</Text>
    </TouchableOpacity>
  ))}
</View>

 <View style={styles.nodeRow}>
  {NODES.map((n) => (
    <View key={n} style={{ flex: 1 }}>
      <TextInput
        style={styles.input}
        placeholder="Interval ms"
        placeholderTextColor="#666"
        keyboardType="numeric"
        value={intervalInputs[n] || ''}
        onChangeText={(text) =>
          setIntervalInputs((prev) => ({
            ...prev,
            [n]: text,
          }))
        }
      />

      <TouchableOpacity 
        style={styles.setButton}
        onPress={() => handleSetInterval(n)}
      >
        <Text style={styles.setButtonText}>
          Set {n}
        </Text>
      </TouchableOpacity>
    </View>
  ))}
</View>

      {/* Per-node cards (the new part) */}
      <ScrollView style={{ flex: 1 }} contentContainerStyle={{ paddingBottom: 20 }}>
        {nodeList.length === 0 ? (
          <Text style={styles.empty}>Waiting for telemetry…</Text>
        ) : (
          nodeList.map((n) => {
            const age   = Date.now() - n.lastSeen;
            const fresh = age < STALE_MS;
            return (
              <View key={n.node} style={styles.card}>
                <View style={styles.cardHeader}>
                  <View style={styles.cardHeaderLeft}>
                    <View style={[styles.statusDot, { backgroundColor: fresh ? '#4ade80' : '#71717a' }]} />
                    <Text style={styles.nodeId}>{n.node}</Text>
                  </View>
                  <Text style={styles.seq}>seq {n.seq}</Text>
                </View>

                <View style={styles.metrics}>
                 <Metric value={(n.temp ?? 0).toFixed(1)}   unit="°C" label="temp" />
                <Metric value={(n.humidity ?? 0).toFixed(0)} unit="%"  label="humidity" />
                <Metric value={(n.lux ?? 0).toFixed(0)}      unit="lx" label="light" />
                </View>

                <Text style={styles.timestamp}>
                  {fresh ? 'live' : `${Math.floor(age / 1000)}s ago`}
                </Text>
              </View>
            );
          })
        )}

        {/* Collapsible-ish raw event log at the bottom */}
        <Text style={styles.sectionTitle}>Recent events</Text>
        {messages.slice(0, 10).map((m, i) => (
          <View key={i} style={styles.msg}>
            <Text style={styles.msgTime}>{m.timestamp}</Text>
            <Text style={styles.msgTopic}>{m.topic}</Text>
            <Text style={styles.msgPayload} numberOfLines={1}>{m.payload}</Text>
          </View>
        ))}
      </ScrollView>
    </View>
  );
}

// Small reusable metric component (still in same file)
function Metric({ value, unit, label }) {
  return (
    <View style={styles.metric}>
      <View style={styles.valueRow}>
        <Text style={styles.value}>{value}</Text>
        <Text style={styles.unit}>{unit}</Text>
      </View>
      <Text style={styles.label}>{label}</Text>
    </View>
  );
}

// ============================================================
// STYL:Array.from(.querySelectorAll('selector'))ES
// ============================================================
const styles = StyleSheet.create({
  container:      { flex: 1, backgroundColor: '#0a0a0f', paddingTop: 60, paddingHorizontal: 20 },

  header:         { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'flex-end', marginBottom: 24 },
  title:          { color: '#fff', fontSize: 32, fontWeight: '700', letterSpacing: 4 },
  subtitle:       { color: '#71717a', fontSize: 13, marginTop: 4 },
  pill:           { flexDirection: 'row', alignItems: 'center', paddingHorizontal: 12, paddingVertical: 6, borderRadius: 999, gap: 8 },
  pillDot:        { width: 8, height: 8, borderRadius: 4 },
  pillText:       { fontSize: 12, fontWeight: '600', textTransform: 'uppercase', letterSpacing: 1 },

  armButton:      { padding: 20, borderRadius: 12, alignItems: 'center', marginBottom: 12 },
  armButtonText:  { color: '#0a0a0f', fontWeight: '700', fontSize: 16 },

  nodeRow:        { flexDirection: 'row', justifyContent: 'space-between', marginBottom: 20, gap: 8 },
  nodeButton:     { flex: 1, backgroundColor: '#1e293b', padding: 12, borderRadius: 8, alignItems: 'center' },
  nodeButtonText: { color: '#94a3b8', fontSize: 12 },

  empty:          { color: '#52525b', fontStyle: 'italic', textAlign: 'center', marginVertical: 30 },

  card:           { backgroundColor: '#16161c', borderRadius: 16, padding: 20, marginBottom: 14, borderWidth: 1, borderColor: '#26262e' },
  cardHeader:     { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginBottom: 20 },
  cardHeaderLeft: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  statusDot:      { width: 10, height: 10, borderRadius: 5 },
  nodeId:         { color: '#fff', fontSize: 18, fontWeight: '600', letterSpacing: 0.5 },
  seq:            { color: '#52525b', fontSize: 11, fontFamily: 'Courier' },
  metrics:        { flexDirection: 'row', justifyContent: 'space-between' },
  metric:         { alignItems: 'flex-start', flex: 1 },
  valueRow:       { flexDirection: 'row', alignItems: 'baseline', gap: 4 },
  value:          { color: '#fff', fontSize: 32, fontWeight: '300', letterSpacing: -1 },
  unit:           { color: '#71717a', fontSize: 14 },
  label:          { color: '#52525b', fontSize: 10, marginTop: 4, textTransform: 'uppercase', letterSpacing: 1.5 },
  timestamp:      { color: '#3f3f46', fontSize: 11, marginTop: 16, textAlign: 'right', fontFamily: 'Courier' },

input: {
  backgroundColor: '#1e293b',
  color: '#fff',
  padding: 8,
  borderRadius: 6,
  marginBottom: 6,
  fontSize: 12,
},

setButton: {
  backgroundColor: '#4ade80',   // bright green so you SEE it
  paddingHorizontal: 16,
  justifyContent: 'center',
  borderRadius: 8,
},

setButtonText: {
  color: '#0a0a0f',
  fontWeight: '700',
},
  sectionTitle:   { color: '#71717a', fontSize: 11, fontWeight: '600', textTransform: 'uppercase', letterSpacing: 1.5, marginTop: 24, marginBottom: 10 },
  msg:            { marginBottom: 8, paddingBottom: 6, borderBottomWidth: 1, borderBottomColor: '#1e1e26' },
  msgTime:        { color: '#52525b', fontSize: 10 },
  msgTopic:       { color: '#4ade80', fontSize: 11, fontFamily: 'monospace' },
  msgPayload:     { color: '#a1a1aa', fontSize: 11, fontFamily: 'monospace' },
});
