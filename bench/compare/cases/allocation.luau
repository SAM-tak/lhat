return function(rounds: number, size: number, seed: number): number
    local total = 0
    for _ = 1, rounds do
        local i = 0
        while i < size do
            local object = {x = i + seed, y = i + seed + 1}
            total += object.x + object.y
            i += 1
        end
    end
    return total
end
