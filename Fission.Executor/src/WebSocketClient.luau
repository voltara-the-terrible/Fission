const httpService, queue = cloneref(game:GetService('HttpService')), {};
const _websocket, websocket = pcall(websocket.connect, 'ws://localhost:7219');

export type DecompileFlags = {
      UpvaluesMode: ('Hidden' | 'Shown'| 'Minimized');
      ConstantsMode: ('Hidden' | 'Shown'| 'Minimized');
      GlobalsMode: ('Hidden' | 'Shown'| 'Minimized');
      ProtosMode: ('Hidden' | 'Shown'| 'Minimized');
      DebugInfo: boolean;
      FunctionInfo: boolean;
      AutoNameVariables: boolean;
      InferRobloxTypes: boolean;
      InferTypes: boolean;
};

const oldDecompile = getgenv().decompile;

getgenv().decompile = function(sourceContainer: LuaSourceContainer, decompileFlags: DecompileFlags?)

      if (decompileFlags == nil) then
            decompileFlags = ({} :: DecompileFlags);

      elseif (not _websocket) then
            
            return oldDecompile and oldDecompile(sourceContainer) or '-- Fission Erorr: Fission is not running.';
      end;

      
      const flags = {};
      
      do
            flags.DebugInfo               = decompileFlags.DebugInfo;
            flags.FunctionInfo            = (decompileFlags.FunctionInfo or true);
            flags.AutoNameVariables       = (decompileFlags.AutoNameVariables or true);
            flags.InferRobloxTypes        = (decompileFlags.InferRobloxTypes or true);
            flags.InferTypes              = (decompileFlags.InferTypes or true);

            const UpvaluesMode = decompileFlags.UpvaluesMode;
            if (UpvaluesMode == 'Shown') then

                  flags.Upvalues = true;
            elseif (UpvaluesMode == 'Minimized') then

                  flags.Upvalues = true;
                  flags.MinifyUpvalues = true;
            elseif (not UpvaluesMode) then

                  flags.Upvalues = true;
                  flags.MinifyUpvalues = true;
            end;

            const ConstantsMode = decompileFlags.ConstantsMode;
            if (ConstantsMode == 'Shown') then
                  flags.Constants = true;

            elseif (ConstantsMode == 'Minimized') then
                  flags.Constants = true;
                  flags.MinifyConstants = true;

            end;

            const GlobalsMode = decompileFlags.ConstantsMode;
            if (GlobalsMode == 'Shown') then
                  flags.Globals = true;
            elseif (GlobalsMode == 'Minimized') then
                  flags.Globals = true;
                  flags.MinifyGlobals = true;    
            end;

            const ProtosMode = decompileFlags.ProtosMode;
            if (ProtosMode == 'Shown') then
                  flags.Protos = true;
            elseif (ProtosMode == 'Minimized') then
                  flags.Protos = true;
                  flags.MinifyProtos = true;    
            end;
      end;

      const success, bytecode = pcall(getscriptbytecode, sourceContainer);
      if (not success) then
            return `-- Failed: [[{bytecode}]]`;
      end;

      const guid = httpService:GenerateGUID(false);
      const message = httpService:JSONEncode({
            guid = guid;
            flags = flags;
            bytecode = crypt.base64encode(bytecode);
      });
      
      const thread = coroutine.running();
      queue[guid] = function(result)
            task.defer(coroutine.resume, thread, result);
      end;

      websocket:Send(message);

      return coroutine.yield();
end;

websocket.OnMessage:Connect(function(sourceContainer: string)
      local response = httpService:JSONDecode(sourceContainer);
      local guid = response.guid;

      if (not queue[guid]) then
            return;
      end;

      queue[guid](response.result);
      queue[guid] = nil;
end);