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
getgenv().decompile = function(arg0: LuaSourceContainer, arg1: DecompileFlags?)
    arg1 = arg1 or {};
    if (not _websocket) then
        return oldDecompile and oldDecompile(arg0) or '-- Fission Erorr: Fission is not running.';
    end;

    const flags = {}; do
        flags.DebugInfo = arg1.DebugInfo;
        flags.FunctionInfo = (arg1.FunctionInfo or true);
        flags.AutoNameVariables = (arg1.AutoNameVariables or true);
        flags.InferRobloxTypes = (arg1.InferRobloxTypes or true);
        flags.InferTypes = (arg1.InferTypes or true);

        const UpvaluesMode = arg1.UpvaluesMode;
        if (UpvaluesMode == 'Shown') then
            flags.Upvalues = true;
        elseif (UpvaluesMode == 'Minimized') then
            flags.Upvalues = true;
            flags.MinifyUpvalues = true;
        elseif (not UpvaluesMode) then
            flags.Upvalues = true;
            flags.MinifyUpvalues = true;
        end;

        const ConstantsMode = arg1.ConstantsMode;
        if (ConstantsMode == 'Shown') then
            flags.Constants = true;
        elseif (ConstantsMode == 'Minimized') then
            flags.Constants = true;
            flags.MinifyConstants = true;    
        end;

        const GlobalsMode = arg1.ConstantsMode;
        if (GlobalsMode == 'Shown') then
            flags.Globals = true;
        elseif (GlobalsMode == 'Minimized') then
            flags.Globals = true;
            flags.MinifyGlobals = true;    
        end;

        const ProtosMode = arg1.ProtosMode;
        if (ProtosMode == 'Shown') then
            flags.Protos = true;
        elseif (ProtosMode == 'Minimized') then
            flags.Protos = true;
            flags.MinifyProtos = true;    
        end;
    end;

    const success, bytecode = pcall(getscriptbytecode, arg0);
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

websocket.OnMessage:Connect(function(arg0: string)
    local response = httpService:JSONDecode(arg0);
    local guid = response.guid;

    if (not queue[guid]) then
        return;
    end;

    queue[guid](response.result);
    queue[guid] = nil;
end);