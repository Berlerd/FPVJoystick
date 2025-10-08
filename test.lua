local input =
  {
    { "Mode", SOURCE},           -- user selects source (typically slider or knob)
    { "Trainer ", SOURCE},       -- user selects source (typically slider or knob)
    { "SMode1  ", SOURCE},       -- user selects source (typically slider or knob)
    { "SMode2  ", SOURCE},       -- user selects source (typically slider or knob)
    { "Interval", VALUE, 0, 100, 0 } -- interval value, default = 0.
  }

local output = { "Val1", "Val2"  }

local function mapSwitch(val)
    if val < -512 then return -100    -- down
    elseif val > 512 then return 100  -- up
    else return 0                     -- center
    end
end

-- Function to scale values to nearest step
local function mapTrainer(val)
    if val >= -100 and val <= -80 then return -100 end
    if val >= -60 and val <= -40 then return -60 end
    if val >= -20 and val <= 0 then return -20 end
    if val >= 20 and val <= 40 then return 20 end
    if val >= 60 and val <= 80 then return 60 end
    if val >= 90 and val <= 100 then return 100 end
    return 0
end

local function init()
  -- Called once when the script is loaded
  print("Lua script initialized")
end

local function run(Mode, Trainer, SMode1 , SMode2 , Interval) -- Must match input table
  local v1 = -100
  local v2 = -100

  Mode   = mapSwitch(Mode)
  Trainer = mapSwitch(Trainer)
  SMode1 = mapSwitch(SMode1)
  SMode2 = mapSwitch(SMode2)

  -- Debug: show input values
  print(string.format("Inputs -> Mode: %d, Trainer: %d, SMode1: %d, SMode2: %d, Interval: %d", Mode, Trainer, SMode1, SMode2, Interval))

  -- Main logic
  if Mode ~= -100 then  -- Joy switch not down
      v1 = -100 -- mapTMode(Trainer)
      print("Mode not down -> v1 = -100")
  else
      -- Joy is down, use SMode logic
      if SMode2 == 0 then          -- SMode2 center
          v1 = -100
          print("SMode2 center -> v1 = -100")
      elseif SMode2 == -100 then   -- SMode2 down
          v1 = -60
          print("SMode2 down -> v1 = -60")
      elseif SMode2 == 100 then    -- SMode2 up
          if SMode1 == 100 then    -- SMode1 up
              v1 = -20
              print("SMode2 up + SMode1 up -> v1 = -20")
          elseif SMode1 == 0 then  -- SMode1 center
              v1 = 20
              print("SMode2 up + SMode1 center -> v1 = 20")
          elseif SMode1 == -100 then -- SMode1 down
              v1 = 60
              print("SMode2 up + SMode1 down -> v1 = 60")
          end
      end
  end

  -- Debug: show output values
  print(string.format("Outputs -> v1: %d, v2: %d", v1, v2))
  
  return v1, v2                       -- Must match output table
end

return { input=input, output=output, run=run, init=init }
