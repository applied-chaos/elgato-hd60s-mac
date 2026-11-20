import CoreMediaIO
import Foundation

let source = ProviderSource(clientQueue: nil)
CMIOExtensionProvider.startService(provider: source.provider)
CFRunLoopRun()
