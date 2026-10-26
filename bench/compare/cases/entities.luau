return function(rounds: number, size: number, seed: number): number
    local total = 0
    for _ = 1, rounds do
        local entities: {{x: number, v: number}} = {}
        local i = 0
        while i < size do entities[i + 1] = {x = i + seed, v = 1}; i += 1 end
        for _ = 1, 32 do
            i = 0
            while i < size do
                local entity = entities[i + 1]
                if entity then
                    entity.x += entity.v
                    total += entity.x
                end
                i += 1
            end
        end
    end
    return total
end
