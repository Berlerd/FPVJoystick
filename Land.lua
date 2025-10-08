-- === Defines ===
-- Input thresholds
local SWITCH_DOWN = -1024
local SWITCH_CENTER_MIN = -204
local SWITCH_CENTER = 0
local SWITCH_CENTER_MAX = 204
local SWITCH_UP = 1024

-- Trainer mapped steps
local TRAINER_MIN = -1024
local TRAINER_LOW = -614
local TRAINER_MIDLOW = -204
local TRAINER_MIDHIGH = 204
local TRAINER_HIGH = 614
local TRAINER_MAX = 1024

-- Output values
local V1_MIN = -1024
local V1_LOW = -614
local V1_MIDLOW = -204
local V1_MID = 0
local V1_MIDHIGH = 204
local V1_HIGH = 614
local V1_MAX = 1024

-- === Inputs ===
local input =
  {
    { "Mode", SOURCE},           -- user selects source (typically slider or knob)
    { "Trainer ", SOURCE},       -- user selects source (typically slider or knob)
    { "Land  ", SOURCE},       -- user selects source (typically slider or knob)
    { "Interval", VALUE, 0, 100, 0 } -- interval value, default = 0.
  }

local output = { "Val1", "Val2"  }

local function mapSwitch(value)
    if value <= SWITCH_CENTER_MIN then
        return SWITCH_DOWN
    elseif value >= SWITCH_CENTER_MAX then
        return SWITCH_UP
    else
        return SWITCH_CENTER
    end
end

-- Function to scale Trainer values to nearest step
local function mapTrainer(val)
    if val >= TRAINER_MIN and val <= TRAINER_LOW then return TRAINER_MIN end
    if val >= TRAINER_LOW+1 and val <= TRAINER_MIDLOW then return TRAINER_LOW end
    if val >= TRAINER_MIDLOW+1 and val <= TRAINER_MIDHIGH then return TRAINER_MIDLOW end
    if val >= TRAINER_MIDHIGH+1 and val <= TRAINER_HIGH then return TRAINER_MIDHIGH end
    if val >= TRAINER_HIGH+1 and val <= TRAINER_MAX then return TRAINER_HIGH end
    if val >= TRAINER_MAX then return TRAINER_MAX end
    return 0
end

local function init()
  print("Lua script initialized")
end

local function run(Mode, Trainer, Land , Interval)
  local v1 = V1_MIN --Retract
  local v2 = V1_MIN --Flaps
  local train = V1_MIN
   
  -- Debug: show input values
  print(string.format("Inputs -> Mode: %d, Trainer: %d, Land: %d, Interval: %d",
      Mode, Trainer, Land, Interval))

  -- Main logic
  local modeSwitch = mapSwitch(Mode)
  if modeSwitch ~= SWITCH_DOWN then  -- Joy switch not down
      train = mapTrainer(Trainer)
      -- Map TLand ranges to outputs
      if train == TRAINER_MIN then
          v1 = V1_MAX
          v2 = V1_MAX
      elseif train == TRAINER_LOW then
          v1 = V1_MAX
          v2 = V1_MIDHIGH
      elseif train == TRAINER_MIDLOW then
          v1 = V1_MAX
          v2 = V1_LOW
      elseif train == TRAINER_MIDHIGH then
          v1 = V1_MIN
          v2 = V1_MAX
      elseif train == TRAINER_HIGH then
          v1 = V1_MIN
          v2 = V1_MIDHIGH
      else
          v1 = V1_MIN
          v2 = V1_LOW
      end	  
      print("Mode not down -> v1 = V1_MIN")
  else
      -- Joy is down, use Land logic
	  Land = mapSwitch(Land)
      if Land == SWITCH_UP then           -- Up
          v1 = V1_MAX
          v2 = V1_MAX
      elseif Land == SWITCH_CENTER then   -- Center
          v1 = V1_MIN
          v2 = V1_MAX
      else                                -- Down (-1)
          v1 = V1_MIN
          v2 = V1_MIDLOW
      end


  end


  -- Debug: show output values
  print(string.format("Outputs -> v1: %d, v2: %d", v1, v2))
  
  return v1, v2
end

return { input=input, output=output, run=run, init=init }
