-- Spinner (hot-reloaded: edit and save while the PHISIKS samples run; state in `self` survives the reload).
speed = 90          -- degrees per second about +Y

function on_start(self)
    self.turns = 0
    log("spinner ready on", self.entity:name())
end

function on_update(self, dt)
    self.entity:rotate(0, 1, 0, speed * dt)
    self.turns = self.turns + speed * dt / 360
end
