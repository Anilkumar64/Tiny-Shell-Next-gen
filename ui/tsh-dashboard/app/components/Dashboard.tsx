"use client";

import React, { useState, useEffect } from 'react';
import { Terminal, Shield, Cpu, Activity, Zap, Lock, Globe, Database, Search } from 'lucide-react';
import { motion } from 'framer-motion';

const Dashboard = () => {
  const [mounted, setMounted] = useState(false);
  const [cmdInput, setCmdInput] = useState('');
  const [stats, setStats] = useState({ nodes: 0, throughput: 0, status: 'CONNECTING', risk: 0, drift: false });
  const [logs, setLogs] = useState([
    { id: 1, type: 'info', msg: 'System initialized. PQC-Ready X25519 established.' },
    { id: 2, type: 'success', msg: 'Secure channel connected to node_4 via TCP.' },
  ]);

  const handleCommand = async (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && cmdInput.trim()) {
      const cmd = cmdInput.trim();
      setCmdInput('');
      setLogs(prev => [...prev, { id: Date.now(), type: 'info', msg: `➜ ${cmd}` }]);
      
      try {
        await fetch(`http://localhost:8080/exec?q=${encodeURIComponent(cmd)}`);
      } catch (err) {
        setLogs(prev => [...prev, { id: Date.now(), type: 'warning', msg: 'Failed to reach API bridge.' }]);
      }
    }
  };

  useEffect(() => {
    setMounted(true);
    // Phase 3: Real-time Telemetry Polling from C++ Backend
    const timer = setInterval(async () => {
      try {
        const res = await fetch('http://localhost:8080');
        const data = await res.json();
        setStats(data);
      } catch (e) {
        console.error("API Bridge Offline");
      }
    }, 1000);
    return () => clearInterval(timer);
  }, []);

  if (!mounted) return null;

  return (
    <div className="min-h-screen bg-black text-green-500 font-mono p-4 grid grid-cols-12 gap-4">
      {/* Sidebar - Nodes & Swarm */}
      <div className="col-span-2 border border-green-900 bg-zinc-950 p-4 rounded-lg flex flex-col gap-4">
        <div className="flex items-center gap-2 text-green-400 font-bold mb-4 border-b border-green-900 pb-2">
          <Globe size={20} />
          <span>SWARM NODES ({stats.nodes})</span>
        </div>
        {[...Array(stats.nodes || 1)].map((_, i) => (
          <div key={i} className="p-2 border border-green-900 rounded bg-black hover:bg-green-950 transition-colors cursor-pointer group">
            <div className="flex justify-between items-center mb-1">
              <span className="text-xs">NODE_0{i+1}</span>
              <div className="w-2 h-2 rounded-full bg-green-500 animate-pulse" />
            </div>
            <div className="text-[10px] text-zinc-500">192.168.1.{100 + i}</div>
          </div>
        ))}
      </div>

      {/* Main Terminal Area */}
      <div className="col-span-7 flex flex-col gap-4">
        <div className="flex-1 border border-green-900 bg-black rounded-lg flex flex-col overflow-hidden relative">
          <div className="bg-zinc-950 border-b border-green-900 p-2 flex items-center justify-between">
            <div className="flex items-center gap-2 text-xs">
              <Terminal size={14} />
              <span>TERMINAL [SECURE_CHANNEL_v2]</span>
            </div>
            <div className="flex gap-2">
              <div className="w-2 h-2 rounded-full bg-red-900" />
              <div className="w-2 h-2 rounded-full bg-yellow-900" />
              <div className="w-2 h-2 rounded-full bg-green-900" />
            </div>
          </div>
          
          <div className="flex-1 p-4 text-sm overflow-y-auto custom-scrollbar">
            <div className="text-zinc-500 mb-2 font-bold">TinyShell NextGen v2.0.0-alpha</div>
            <div className="mb-1"><span className="text-blue-500">➜</span> <span className="text-zinc-400">~</span> <span className="text-green-400">? analyze all root logs</span></div>
            <div className="text-zinc-400 mb-4 ml-4">
              <span className="text-blue-400">[AI Context]</span> Analyzing intent...<br/>
              <span className="text-blue-400">[AI Context]</span> Formulating command: <span className="text-yellow-500 italic">find /var/log -user root -mtime -7</span>
            </div>
            <div className="mb-1"><span className="text-blue-500">➜</span> <span className="text-zinc-400">~</span> <span className="text-green-400">stego:upload_payload</span></div>
            <div className="text-zinc-400 mb-4 ml-4">
              <span className="text-purple-400">[Stealth]</span> Carrier image detected (JPEG).<br/>
              <span className="text-purple-400">[Stealth]</span> Injecting secret payload into EOF tagging segment...<br/>
              <span className="text-green-500">[Success]</span> Payload delivery concealed.
            </div>
            <div className="mb-1"><span className="text-blue-500">➜</span> <span className="text-zinc-400">~</span> <span className="animate-pulse inline-block w-2 h-4 bg-green-500 align-middle ml-1" /></div>
          </div>

          <div className="p-2 border-t border-green-900 bg-zinc-950 flex items-center gap-2">
            <span className="text-blue-500 font-bold text-xs">➜</span>
            <input 
              type="text" 
              value={cmdInput}
              onChange={(e) => setCmdInput(e.target.value)}
              onKeyDown={handleCommand}
              className="bg-transparent border-none outline-none text-green-400 w-full text-xs" 
              placeholder="Enter command or intent (prefix with ? for AI)..."
              autoFocus
            />
          </div>
        </div>

        {/* Audit Log Bottom */}
        <div className="h-40 border border-green-900 bg-zinc-950 rounded-lg flex flex-col overflow-hidden">
          <div className="bg-green-900/10 p-2 text-[10px] flex items-center gap-2 border-b border-green-900/50">
            <Lock size={12} />
            <span>IMMUTABLE AUDIT LEDGER (SHA-256 SIGNED)</span>
          </div>
          <div className="flex-1 p-2 overflow-y-auto text-[10px]">
            {mounted && logs.map((log) => (
              <div key={log.id} className="flex gap-2 mb-1">
                <span className="text-zinc-600">[{new Date().toLocaleTimeString()}]</span>
                <span className={log.type === 'success' ? 'text-green-400' : log.type === 'warning' ? 'text-yellow-600' : 'text-blue-400'}>
                  {log.msg}
                </span>
              </div>
            ))}
          </div>
        </div>
      </div>

      {/* Right Sidebar - AI & System Stats */}
      <div className="col-span-3 flex flex-col gap-4">
        <div className="border border-green-900 bg-zinc-950 rounded-lg p-4">
          <div className="flex items-center gap-2 text-green-400 font-bold mb-4 text-sm border-b border-green-900 pb-2">
            <Zap size={18} />
            <span>AI PREDICTIVE INSIGHTS</span>
          </div>
          <div className="space-y-4">
            {stats.drift && (
              <div className="text-xs bg-red-950/50 p-2 border-l-2 border-red-500 rounded animate-pulse">
                <div className="font-bold mb-1 text-red-500">INTENT DRIFT DETECTED</div>
                <p className="text-red-300">Cognitive fatigue or malicious automation suspected. Pipeline execution cooling down.</p>
              </div>
            )}
            <div className="text-xs bg-black p-2 border-l-2 border-orange-500 rounded">
              <div className="font-bold mb-1 text-orange-500">GLOBAL PIPELINE RISK</div>
              <div className="flex items-center gap-2">
                <div className="flex-1 bg-zinc-900 rounded-full h-2">
                  <div className="bg-orange-500 h-2 rounded-full" style={{ width: `${stats.risk}%` }}></div>
                </div>
                <span className="text-orange-400 font-bold">{stats.risk}/100</span>
              </div>
            </div>
            <div className="text-xs bg-black p-2 border-l-2 border-yellow-500 rounded">
              <div className="font-bold mb-1 text-yellow-500">PREDICTIVE PRE-FETCH</div>
              <p className="text-zinc-400">Operator is searching for logs. Silently pre-caching <span className="text-green-500">journalctl</span> buffers for faster execution.</p>
            </div>
            <div className="text-xs bg-black p-2 border-l-2 border-blue-500 rounded">
              <div className="font-bold mb-1 text-blue-500">TRANSLATION READY</div>
              <p className="text-zinc-400">Target is Linux 6.8. Detected PowerShell-style input. Ready to translate to native syscalls.</p>
            </div>
          </div>
        </div>

        <div className="border border-green-900 bg-zinc-950 rounded-lg p-4 flex-1">
          <div className="flex items-center gap-2 text-green-400 font-bold mb-4 text-sm border-b border-green-900 pb-2">
            <Activity size={18} />
            <span>SYSTEM HOLOGRAPH</span>
          </div>
          <div className="space-y-2 text-[10px]">
             <div className="flex justify-between items-center bg-black p-2 border border-green-900/30 rounded">
               <span>PQC ROTATION</span>
               <span className="text-green-500">{stats.status}</span>
             </div>
             <div className="flex justify-between items-center bg-black p-2 border border-green-900/30 rounded">
               <span>THROUGHPUT</span>
               <span className="text-green-500">{stats.throughput} MB/s</span>
             </div>
             <div className="flex justify-between items-center bg-black p-2 border border-green-900/30 rounded">
               <span>WASM SANDBOX</span>
               <span className="text-green-500">SECURE</span>
             </div>
             
             <div className="mt-4 pt-4 border-t border-green-900/30">
               <div className="text-green-800 mb-2 uppercase tracking-widest text-[8px]">Network Topology</div>
               <div className="h-32 bg-green-500/5 rounded border border-green-900/20 flex items-center justify-center relative overflow-hidden">
                  <div className="absolute inset-0 bg-[radial-gradient(circle,rgba(34,197,94,0.1)_1px,transparent_1px)] bg-[size:10px_10px]" />
                  <div className="w-12 h-12 rounded-full border border-green-500/50 flex items-center justify-center animate-pulse">
                    <Shield size={24} className="text-green-500/80" />
                  </div>
                  {/* Mock orbital nodes */}
                  <div className="absolute top-4 left-10 w-2 h-2 rounded-full bg-green-500" />
                  <div className="absolute bottom-10 right-8 w-2 h-2 rounded-full bg-green-500" />
                  <div className="absolute top-1/2 left-20 w-1 h-1 rounded-full bg-green-500/30" />
               </div>
             </div>
          </div>
        </div>
      </div>

      <style jsx global>{`
        .custom-scrollbar::-webkit-scrollbar { width: 4px; }
        .custom-scrollbar::-webkit-scrollbar-track { background: transparent; }
        .custom-scrollbar::-webkit-scrollbar-thumb { background: #064e3b; border-radius: 2px; }
        .custom-scrollbar::-webkit-scrollbar-thumb:hover { background: #059669; }
      `}</style>
    </div>
  );
};

export default Dashboard;
